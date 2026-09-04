#include "parser.h"
#include "error/error_reporter.h"
#include "ir/ir.h"
#include "string_intern.h"
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PARSER_ERROR_BUF_SIZE 512
#define PARSER_SCRATCH_BUF_SIZE 1024

static void parser_skip_chain_continuation(Parser *parser) {
  while (parser->current_token.type == TOKEN_NEWLINE &&
         parser->peek_token.type == TOKEN_NEWLINE) {
    parser_advance(parser);
  }
  if (parser->current_token.type == TOKEN_NEWLINE &&
      (parser->peek_token.type == TOKEN_DOT ||
       parser->peek_token.type == TOKEN_ARROW)) {
    parser_advance(parser);
  }
}

static int parser_at_contextual_keyword(Parser *parser, const char *word,
                                        TokenType following) {
  return parser_is_identifier_like(parser->current_token.type) &&
         parser->current_token.value &&
         strcmp(parser->current_token.value, word) == 0 &&
         parser->peek_token.type == following;
}

static void parser_report_lexer_token_error(Parser *parser,
                                            const Token *token) {
  if (!parser || !token || token->type != TOKEN_ERROR) {
    return;
  }

  const char *message = (token->value && token->value[0] != '\0')
                            ? token->value
                            : "Invalid token";
  char error_msg[PARSER_ERROR_BUF_SIZE];
  snprintf(error_msg, sizeof(error_msg), "Lexical error: %s", message);

  parser->has_error = 1;
  parser->had_error = 1;
  parser->saw_lexical_error = 1;
  parser->error_count++;
  free(parser->error_message);
  parser->error_message = strdup(error_msg);

  if (parser->error_reporter) {
    SourceLocation location =
        source_location_create(token->line, token->column);
    location.filename = parser->source_filename;
    error_reporter_add_error(parser->error_reporter, ERROR_LEXICAL, location,
                             error_msg);
  }
}

static SourceLocation parser_current_location(Parser *parser) {
  SourceLocation location = source_location_create(
      parser ? parser->current_token.line : 0,
      parser ? parser->current_token.column : 0);
  location.filename = parser ? parser->source_filename : NULL;
  return location;
}

Parser *parser_create(Lexer *lexer) {
  return parser_create_with_error_reporter(lexer, NULL);
}

Parser *parser_create_with_error_reporter(Lexer *lexer,
                                          ErrorReporter *error_reporter) {
  if (!lexer) {
    return NULL;
  }

  Parser *parser = malloc(sizeof(Parser));
  if (!parser)
    return NULL;

  parser->lexer = lexer;
  parser->current_token = lexer_next_token(lexer);
  parser->peek_token = lexer_next_token(lexer);
  parser->has_error = 0;
  parser->had_error = 0;
  parser->error_count = 0;
  parser->error_message = NULL;
  parser->error_reporter = error_reporter;
  parser->error_recovery_mode = 0;
  parser->previous_token_type = TOKEN_EOF;
  parser->previous_token_text[0] = '\0';
  parser->brace_depth = 0;
  parser->group_context = NULL;
  parser->recover_at_body_brace = 0;
  parser->saw_lexical_error = 0;
  parser->source_filename = error_reporter_current_filename(error_reporter);
  parser->gpu_mode = 0;
  parser->comptime_depth = 0;
  parser->pending_composed_name = NULL;
  parser->expression_depth = 0;
  parser->extra_declarations[0] = NULL;
  parser->extra_declarations[1] = NULL;
  parser->extra_declaration_count = 0;

  if (parser->current_token.type == TOKEN_ERROR) {
    parser_report_lexer_token_error(parser, &parser->current_token);
  } else if (parser->peek_token.type == TOKEN_ERROR) {
    parser_report_lexer_token_error(parser, &parser->peek_token);
  }

  return parser;
}

void parser_destroy(Parser *parser) {
  if (parser) {
    token_destroy(&parser->current_token);
    token_destroy(&parser->peek_token);
    ast_destroy_node(parser->pending_composed_name);
    for (size_t i = 0; i < parser->extra_declaration_count; i++) {
      ast_destroy_node(parser->extra_declarations[i]);
    }
    free(parser->error_message);
    free(parser);
  }
}

void parser_advance(Parser *parser) {
  if (!parser || parser->current_token.type == TOKEN_EOF)
    return;

  parser->previous_token_type = parser->current_token.type;
  parser->previous_token_text[0] = '\0';
  if (parser->current_token.value) {
    snprintf(parser->previous_token_text, sizeof(parser->previous_token_text),
             "%s", parser->current_token.value);
  }
  if (parser->current_token.type == TOKEN_LBRACE)
    parser->brace_depth++;
  else if (parser->current_token.type == TOKEN_RBRACE && parser->brace_depth > 0)
    parser->brace_depth--;

  token_destroy(&parser->current_token);
  parser->current_token = parser->peek_token;

  // Clear peek_token to avoid double-free
  parser->peek_token.type = TOKEN_EOF;
  parser->peek_token.value = NULL;
  parser->peek_token.lexeme.data = NULL;
  parser->peek_token.lexeme.length = 0;
  parser->peek_token.line = 0;
  parser->peek_token.column = 0;
  parser->peek_token.is_interned = 0;

  // Get new peek token
  parser->peek_token = lexer_next_token(parser->lexer);

  if (parser->current_token.type == TOKEN_ERROR) {
    parser_report_lexer_token_error(parser, &parser->current_token);
  }
}

int parser_match(Parser *parser, TokenType type) {
  if (!parser)
    return 0;
  return parser->current_token.type == type;
}

static const char *token_type_to_string(TokenType type) {
  switch (type) {
  case TOKEN_EOF:
    return "end of file";
  case TOKEN_IDENTIFIER:
    return "identifier";
  case TOKEN_NUMBER:
    return "number";
  case TOKEN_STRING:
    return "string";
  case TOKEN_IMPORT:
    return "'import'";
  case TOKEN_IMPORT_STR:
    return "'import_str'";
  case TOKEN_EXTERN:
    return "'extern'";
  case TOKEN_EXPORT:
    return "'export'";
  case TOKEN_VAR:
    return "'var'";
  case TOKEN_WORKGROUP:
    return "'workgroup'";
  case TOKEN_PRIVATE:
    return "'private'";
  case TOKEN_BARRIER:
    return "'barrier'";
  case TOKEN_FUNCTION:
  case TOKEN_FN:
    return "'fn'";
  case TOKEN_STRUCT:
    return "'struct'";
  case TOKEN_METHOD:
    return "'method'";
  case TOKEN_RETURN:
    return "'return'";
  case TOKEN_IF:
    return "'if'";
  case TOKEN_ELSE:
    return "'else'";
  case TOKEN_WHILE:
    return "'while'";
  case TOKEN_FOR:
    return "'for'";
  case TOKEN_SWITCH:
    return "'switch'";
  case TOKEN_CASE:
    return "'case'";
  case TOKEN_DEFAULT:
    return "'default'";
  case TOKEN_BREAK:
    return "'break'";
  case TOKEN_CONTINUE:
    return "'continue'";
  case TOKEN_DEFER:
    return "'defer'";
  case TOKEN_ERRDEFER:
    return "'errdefer'";
  case TOKEN_ASM:
    return "'asm'";
  case TOKEN_THIS:
    return "'this'";
  case TOKEN_NEW:
    return "'new'";
  case TOKEN_COLON:
    return "':'";
  case TOKEN_SEMICOLON:
    return "';'";
  case TOKEN_COMMA:
    return "','";
  case TOKEN_EQUALS:
    return "'='";
  case TOKEN_ARROW:
    return "'->'";
  case TOKEN_LPAREN:
    return "'('";
  case TOKEN_RPAREN:
    return "')'";
  case TOKEN_LBRACE:
    return "'{'";
  case TOKEN_RBRACE:
    return "'}'";
  case TOKEN_LBRACKET:
    return "'['";
  case TOKEN_RBRACKET:
    return "']'";
  case TOKEN_PLUS:
    return "'+'";
  case TOKEN_MINUS:
    return "'-'";
  case TOKEN_PLUS_PLUS:
    return "'++'";
  case TOKEN_MINUS_MINUS:
    return "'--'";
  case TOKEN_MULTIPLY:
    return "'*'";
  case TOKEN_AMPERSAND:
    return "'&'";
  case TOKEN_PIPE:
    return "'|'";
  case TOKEN_CARET:
    return "'^'";
  case TOKEN_LSHIFT:
    return "'<<'";
  case TOKEN_RSHIFT:
    return "'>>'";
  case TOKEN_TILDE:
    return "'~'";
  case TOKEN_AND_AND:
    return "'&&'";
  case TOKEN_OR_OR:
    return "'||'";
  case TOKEN_DIVIDE:
    return "'/'";
  case TOKEN_PERCENT:
    return "'%'";
  case TOKEN_DOT:
    return "'.'";
  case TOKEN_NEWLINE:
    return "newline";
  case TOKEN_ERROR:
    return "lexical error";
  default:
    return "unknown token";
  }
}

int parser_expect_statement_end(Parser *parser) {
  if (parser_match(parser, TOKEN_SEMICOLON) ||
      parser_match(parser, TOKEN_NEWLINE)) {
    parser_advance(parser);
    return 1;
  }

  // Skip over any extra newlines
  while (parser_match(parser, TOKEN_NEWLINE)) {
    parser_advance(parser);
  }

  if (parser_match(parser, TOKEN_SEMICOLON)) {
    parser_advance(parser);
    return 1;
  }

  if (parser_match(parser, TOKEN_PLUS_PLUS) ||
      parser_match(parser, TOKEN_MINUS_MINUS)) {
    parser_set_error(parser,
                     "'++' and '--' are statements, not expressions: they "
                     "produce no value to use here");
    return 0;
  }

  parser_set_error(parser,
                   "Expected ';' or newline at the end of the statement");
  return 0;
}

int parser_expect(Parser *parser, TokenType type) {
  if (!parser)
    return 0;

  if (parser->current_token.type == type) {
    parser_advance(parser);
    return 1;
  }

  char error_msg[PARSER_ERROR_BUF_SIZE];
  const char *expected_str = token_type_to_string(type);
  const char *actual_str = token_type_to_string(parser->current_token.type);

  snprintf(error_msg, sizeof(error_msg), "Expected %s, found %s", expected_str,
           actual_str);

  // Generate context-specific suggestions
  const char *suggestion = NULL;
  char help_buf[PARSER_ERROR_BUF_SIZE];
  if (type == TOKEN_SEMICOLON) {
    suggestion = "add a semicolon ';' to end the statement";
  } else if (type == TOKEN_RPAREN &&
             parser->current_token.type == TOKEN_COMMA) {
    /* A comma where ')' was due means one of two different mistakes, and the
       parser knows which: a list that ran on, or parentheses asked to hold
       more than the one value they can hold. Mettle has no comma operator
       and no tuples, so the second is never a list at all. */
    const char *ctx = parser->group_context;
    if (ctx && (strcmp(ctx, "parameter list") == 0 ||
                strcmp(ctx, "argument list") == 0)) {
      snprintf(help_buf, sizeof(help_buf),
               "the %s ends here; drop the trailing ',' or close the list", ctx);
    } else if (ctx) {
      snprintf(help_buf, sizeof(help_buf),
               "the %s holds one value, and Mettle has no tuples; remove the "
               "',' and everything after it",
               ctx);
    } else {
      snprintf(help_buf, sizeof(help_buf),
               "parentheses group one value, and Mettle has no tuples; remove "
               "the ',' and everything after it");
    }
    suggestion = help_buf;
  } else if (type == TOKEN_RBRACE && parser->current_token.type == TOKEN_EOF) {
    suggestion = "add a closing brace '}' to match the opening brace";
  } else if (type == TOKEN_COLON &&
             parser->current_token.type == TOKEN_EQUALS) {
    suggestion = "use ':' for type annotations, '=' for assignments";
  }

  parser_set_error_with_suggestion(parser, error_msg, suggestion);
  return 0;
}

void parser_set_error(Parser *parser, const char *message) {
  parser_set_error_with_suggestion(parser, message, NULL);
}

void parser_set_error_with_suggestion(Parser *parser, const char *message,
                                      const char *suggestion) {
  if (!parser || !message)
    return;

  parser->has_error = 1;
  parser->had_error = 1;
  parser->error_count++;
  free(parser->error_message);
  parser->error_message = strdup(message);

  // A bad token leaves the stream unreliable, so every grammar complaint after
  // it is a guess. The lexical error already names the real problem.
  if (parser->saw_lexical_error)
    return;

  // If we have an error reporter, add the error to it
  if (parser->error_reporter) {
    SourceLocation location = source_location_create(
        parser->current_token.line, parser->current_token.column);
    location.filename = parser->source_filename;

    size_t span_len = 1;
    if (parser->current_token.lexeme.length > 0) {
      span_len = parser->current_token.lexeme.length;
    } else if (parser->current_token.value &&
               parser->current_token.value[0] != '\0') {
      span_len = strlen(parser->current_token.value);
    }
    SourceSpan span = source_span_from_location(location, span_len);

    if (suggestion) {
      error_reporter_add_error_with_span_and_suggestion(
          parser->error_reporter, ERROR_SYNTAX, span, message, suggestion);
    } else {
      // Try to generate a helpful suggestion
      const char *auto_suggestion = NULL;
      if (parser->current_token.value) {
        auto_suggestion =
            error_reporter_suggest_for_token(parser->current_token.value);
      }

      if (auto_suggestion) {
        error_reporter_add_error_with_span_and_suggestion(
            parser->error_reporter, ERROR_SYNTAX, span, message,
            auto_suggestion);
      } else {
        error_reporter_add_error_with_span(parser->error_reporter, ERROR_SYNTAX,
                                           span, message);
      }
    }
  }
}

void parser_refine_error(Parser *parser, const char *message) {
  if (!parser || !message)
    return;
  free(parser->error_message);
  parser->error_message = strdup(message);
  if (parser->error_reporter)
    error_reporter_refine_last(parser->error_reporter, message);
}

void parser_recover_from_error(Parser *parser) {
  if (!parser)
    return;

  parser->error_recovery_mode = 1;
  parser_synchronize(parser);

  // Clear error state to continue parsing
  parser->has_error = 0;
  free(parser->error_message);
  parser->error_message = NULL;
  parser->error_recovery_mode = 0;
}

void parser_synchronize(Parser *parser) {
  if (!parser)
    return;

  while (parser->current_token.type != TOKEN_EOF) {
    if (parser->current_token.type == TOKEN_SEMICOLON ||
        parser->current_token.type == TOKEN_NEWLINE) {
      parser_advance(parser);
      return;
    }

    // Synchronize on the current token, not peek. Using peek can return
    // without consuming an invalid current token (e.g. current '=>', peek
    // 'return'), causing parse/recover loops to spin forever at top level.
    switch (parser->current_token.type) {
    case TOKEN_FUNCTION:
    case TOKEN_FN:
    case TOKEN_VAR:
    case TOKEN_WORKGROUP:
    case TOKEN_PRIVATE:
    case TOKEN_BARRIER:
    case TOKEN_STRUCT:
    case TOKEN_RETURN:
    case TOKEN_IF:
    case TOKEN_WHILE:
    case TOKEN_FOR:
    case TOKEN_SWITCH:
    case TOKEN_MATCH:
    case TOKEN_BREAK:
    case TOKEN_CONTINUE:
    case TOKEN_RBRACE:
      return;
    default:
      break;
    }

    parser_advance(parser);
  }
}

void parser_recover_in_block(Parser *parser, int block_depth) {
  if (!parser)
    return;

  parser->error_recovery_mode = 1;
  while (parser->current_token.type != TOKEN_EOF) {
    if (parser->brace_depth <= block_depth) {
      // The block's own closing brace ends the search; the block parser
      // consumes it, so recovery must not.
      if (parser->current_token.type == TOKEN_RBRACE)
        break;
      // A '{' here opens the body of the construct whose header just failed.
      // Stopping in front of it lets that body parse as an ordinary block, so
      // the statements inside are still checked and the header costs one
      // diagnostic.
      if (parser->current_token.type == TOKEN_LBRACE)
        break;
      // A broken loop header holds semicolons of its own (`for i = 0; i < n;`),
      // so stopping at the first one would drop the rest of the header into
      // the block as statements. Run to the body brace instead.
      if (!parser->recover_at_body_brace &&
          (parser->current_token.type == TOKEN_SEMICOLON ||
           parser->current_token.type == TOKEN_NEWLINE)) {
        parser_advance(parser);
        break;
      }
    }
    parser_advance(parser);
  }
  parser->error_recovery_mode = 0;
  parser->group_context = NULL;
  parser->recover_at_body_brace = 0;
}

// Tokens that begin a top-level item. Recovery at file scope stops on one of
// these so the rest of a broken function body is never read as declarations.
static int parser_token_starts_declaration(TokenType type) {
  switch (type) {
  case TOKEN_IMPORT:
  case TOKEN_EXPORT:
  case TOKEN_EXTERN:
  case TOKEN_FN:
  case TOKEN_FUNCTION:
  case TOKEN_KERNEL:
  case TOKEN_STRUCT:
  case TOKEN_ENUM:
  case TOKEN_TRAIT:
  case TOKEN_IMPL:
  case TOKEN_VAR:
  case TOKEN_CONST:
  case TOKEN_AT:
    return 1;
  default:
    return 0;
  }
}

void parser_recover_to_declaration(Parser *parser) {
  if (!parser)
    return;

  parser->error_recovery_mode = 1;
  while (parser->current_token.type != TOKEN_EOF) {
    if (parser->brace_depth == 0 &&
        parser_token_starts_declaration(parser->current_token.type))
      break;
    parser_advance(parser);
  }
  parser->error_recovery_mode = 0;
  parser->group_context = NULL;

  parser->has_error = 0;
  free(parser->error_message);
  parser->error_message = NULL;
}

int parser_get_operator_precedence(TokenType type) {
  switch (type) {
  case TOKEN_DOT:
    return 13; // Member access (highest precedence)
  case TOKEN_MULTIPLY:
  case TOKEN_DIVIDE:
  case TOKEN_PERCENT:
    return 11; // Multiplicative
  case TOKEN_PLUS:
  case TOKEN_MINUS:
    return 10; // Additive
  case TOKEN_LSHIFT:
  case TOKEN_RSHIFT:
    return 9; // Shift
  case TOKEN_LESS_THAN:
  case TOKEN_LESS_EQUALS:
  case TOKEN_GREATER_THAN:
  case TOKEN_GREATER_EQUALS:
    return 8; // Relational
  case TOKEN_EQUALS_EQUALS:
  case TOKEN_NOT_EQUALS:
    return 7; // Equality
  case TOKEN_AMPERSAND:
    return 6; // Bitwise AND
  case TOKEN_CARET:
    return 5; // Bitwise XOR
  case TOKEN_PIPE:
    return 4; // Bitwise OR
  case TOKEN_AND_AND:
    return 3; // Logical AND
  case TOKEN_OR_OR:
    return 2; // Logical OR
  default:
    return 0; // Not a binary operator
  }
}

int parser_is_binary_operator(TokenType type) {
  switch (type) {
  case TOKEN_PLUS:
  case TOKEN_MINUS:
  case TOKEN_MULTIPLY:
  case TOKEN_DIVIDE:
  case TOKEN_PERCENT:
  case TOKEN_EQUALS_EQUALS:
  case TOKEN_NOT_EQUALS:
  case TOKEN_LESS_THAN:
  case TOKEN_LESS_EQUALS:
  case TOKEN_GREATER_THAN:
  case TOKEN_GREATER_EQUALS:
  case TOKEN_AND_AND:
  case TOKEN_OR_OR:
  case TOKEN_AMPERSAND:
  case TOKEN_PIPE:
  case TOKEN_CARET:
  case TOKEN_LSHIFT:
  case TOKEN_RSHIFT:
  case TOKEN_DOT:
    return 1;
  default:
    return 0;
  }
}

/* An operator that may open a continuation line, so
 *
 *   var wx: float64 = x + a(i)
 *                       + b(i);
 *
 * reads as one expression. It is the mirror of the trailing form, which works
 * because the loop below skips newlines after consuming an operator.
 *
 * Every operator here is one no statement can begin with, so nothing that used
 * to parse as two statements now parses as one. `*` is the exception and is
 * deliberately absent: `*p = 5;` is a statement, so a line opening with `*`
 * is genuinely ambiguous. Multiplication splits on the trailing form. */
static int parser_operator_opens_continuation_line(TokenType type) {
  switch (type) {
  case TOKEN_PLUS:
  case TOKEN_MINUS:
  case TOKEN_DIVIDE:
  case TOKEN_PERCENT:
  case TOKEN_EQUALS_EQUALS:
  case TOKEN_NOT_EQUALS:
  case TOKEN_LESS_THAN:
  case TOKEN_LESS_EQUALS:
  case TOKEN_GREATER_THAN:
  case TOKEN_GREATER_EQUALS:
  case TOKEN_AND_AND:
  case TOKEN_OR_OR:
  case TOKEN_AMPERSAND:
  case TOKEN_PIPE:
  case TOKEN_CARET:
  case TOKEN_LSHIFT:
  case TOKEN_RSHIFT:
    return 1;
  default:
    return 0;
  }
}

int parser_is_unary_operator(TokenType type) {
  switch (type) {
  case TOKEN_MINUS:
  case TOKEN_PLUS:
  case TOKEN_MULTIPLY:
  case TOKEN_AMPERSAND:
  case TOKEN_TILDE:
  case TOKEN_NOT:
    return 1;
  default:
    return 0;
  }
}

ASTNode *parser_parse_program(Parser *parser) {
  if (!parser)
    return NULL;

  ASTNode *program = ast_create_program();
  if (!program)
    return NULL;

  Program *prog_data = (Program *)program->data;

  while (parser->current_token.type != TOKEN_EOF) {
    size_t token_pos_before = parser->lexer->position;

    // Skip empty statements/newlines at the top level
    if (parser->current_token.type == TOKEN_NEWLINE ||
        parser->current_token.type == TOKEN_SEMICOLON) {
      parser_advance(parser);
      continue;
    }

    ASTNode *declaration = parser_parse_declaration(parser);

    if (declaration) {
      // Add to program's declarations array
      prog_data->declarations =
          realloc(prog_data->declarations,
                  (prog_data->declaration_count + 1) * sizeof(ASTNode *));
      if (prog_data->declarations) {
        prog_data->declarations[prog_data->declaration_count] = declaration;
        prog_data->declaration_count++;
        ast_add_child(program, declaration);
      }
      for (size_t e = 0; e < parser->extra_declaration_count; e++) {
        ASTNode *extra = parser->extra_declarations[e];
        parser->extra_declarations[e] = NULL;
        prog_data->declarations =
            realloc(prog_data->declarations,
                    (prog_data->declaration_count + 1) * sizeof(ASTNode *));
        if (prog_data->declarations) {
          prog_data->declarations[prog_data->declaration_count] = extra;
          prog_data->declaration_count++;
          ast_add_child(program, extra);
        }
      }
      parser->extra_declaration_count = 0;
    } else if (!parser->has_error) {
      parser_set_error(parser, "Failed to parse declaration");
      parser_advance(parser);
    }

    if (parser->has_error) {
      parser_recover_to_declaration(parser);
      // Hard guard against non-advancing recovery loops.
      if (parser->current_token.type != TOKEN_EOF &&
          parser->lexer->position == token_pos_before) {
        parser_advance(parser);
      }
    }
  }

  return program;
}

static ASTNode *parser_parse_extern_var_declaration(Parser *parser);

// Flags collected from a run of `@ident[!]` decorators.
typedef struct {
  int is_inline;          // `@inline`
  int is_inline_contract; // `@inline!` (implies is_inline)
  int is_noinline;        // `@noinline`
  int is_pure;            // `@pure`
  int is_noalloc;         // `@noalloc`
  int is_test;            // `@test`: compile-time unit test (mettle test)
  int is_swappable;       // `@swappable`: may be replaced at a `quiesce` point
  int is_naked;
  int is_interrupt;
  int is_rule;
  int simd_mode; // SimdAttr from `@simd` / `@simd!` (SIMD_ATTR_NONE if absent)
  int unroll_factor; // `@unroll(n)` on a loop; 0 if absent
} ParsedDecorators;

// Consume a run of `@ident[!]` decorators into `out`. Assumes the current token
// is TOKEN_AT. Returns 1 on success (parser positioned on the decorated
// construct), 0 on error (a parser error is set). Recognizes `@inline` /
// `@inline!`, `@noinline`, `@pure`, `@noalloc`, and `@simd` / `@simd!`;
// rejects unknown names, duplicates, and the `@inline`+`@noinline` conflict.
static int parser_parse_decorator_chain(Parser *parser, ParsedDecorators *out) {
  out->is_inline = 0;
  out->is_inline_contract = 0;
  out->is_noinline = 0;
  out->is_pure = 0;
  out->is_noalloc = 0;
  out->is_test = 0;
  out->is_swappable = 0;
  out->is_naked = 0;
  out->is_interrupt = 0;
  out->is_rule = 0;
  out->simd_mode = SIMD_ATTR_NONE;
  out->unroll_factor = 0;

  while (parser->current_token.type == TOKEN_AT) {
    parser_advance(parser); // consume '@'
    if (!parser_is_identifier_like(parser->current_token.type)) {
      parser_set_error(parser,
                       "Expected a decorator name after '@' (one of 'inline', "
                       "'noinline', 'pure', 'noalloc', 'simd', 'swappable')");
      return 0;
    }
    const char *name = parser->current_token.value;
    if (strcmp(name, "inline") == 0) {
      if (out->is_inline) {
        parser_set_error(parser, "Duplicate '@inline' decorator");
        return 0;
      }
      out->is_inline = 1;
      parser_advance(parser);
      if (parser->current_token.type == TOKEN_NOT) {
        out->is_inline_contract = 1; // `@inline!`: contract, not just a hint
        parser_advance(parser);      // consume '!'
      }
    } else if (strcmp(name, "noalloc") == 0) {
      if (out->is_noalloc) {
        parser_set_error(parser, "Duplicate '@noalloc' decorator");
        return 0;
      }
      out->is_noalloc = 1;
      parser_advance(parser);
    } else if (strcmp(name, "noinline") == 0) {
      if (out->is_noinline) {
        parser_set_error(parser, "Duplicate '@noinline' decorator");
        return 0;
      }
      out->is_noinline = 1;
      parser_advance(parser);
    } else if (strcmp(name, "pure") == 0) {
      if (out->is_pure) {
        parser_set_error(parser, "Duplicate '@pure' decorator");
        return 0;
      }
      out->is_pure = 1;
      parser_advance(parser);
    } else if (strcmp(name, "test") == 0) {
      if (out->is_test) {
        parser_set_error(parser, "Duplicate '@test' decorator");
        return 0;
      }
      out->is_test = 1;
      parser_advance(parser);
    } else if (strcmp(name, "swappable") == 0) {
      if (out->is_swappable) {
        parser_set_error(parser, "Duplicate '@swappable' decorator");
        return 0;
      }
      out->is_swappable = 1;
      parser_advance(parser);
    } else if (strcmp(name, "naked") == 0) {
      if (out->is_naked) {
        parser_set_error(parser, "Duplicate '@naked' decorator");
        return 0;
      }
      out->is_naked = 1;
      parser_advance(parser);
    } else if (strcmp(name, "interrupt") == 0) {
      if (out->is_interrupt) {
        parser_set_error(parser, "Duplicate '@interrupt' decorator");
        return 0;
      }
      out->is_interrupt = 1;
      parser_advance(parser);
    } else if (strcmp(name, "rule") == 0) {
      if (out->is_rule) {
        parser_set_error(parser, "Duplicate '@rule' decorator");
        return 0;
      }
      out->is_rule = 1;
      parser_advance(parser);
    } else if (strcmp(name, "simd") == 0) {
      if (out->simd_mode != SIMD_ATTR_NONE) {
        parser_set_error(parser, "Duplicate '@simd' decorator");
        return 0;
      }
      parser_advance(parser); // consume 'simd'
      out->simd_mode = SIMD_ATTR_HINT;
      if (parser->current_token.type == TOKEN_NOT) {
        out->simd_mode = SIMD_ATTR_CONTRACT;
        parser_advance(parser); // consume '!'
      }
    } else if (strcmp(name, "unroll") == 0) {
      if (out->unroll_factor) {
        parser_set_error(parser, "Duplicate '@unroll' decorator");
        return 0;
      }
      parser_advance(parser); // consume 'unroll'
      if (!parser_expect(parser, TOKEN_LPAREN)) {
        parser_set_error(parser, "Expected '(factor)' after '@unroll'");
        return 0;
      }
      if (parser->current_token.type != TOKEN_NUMBER ||
          strchr(parser->current_token.value, '.')) {
        parser_set_error(parser,
                         "Expected an integer unroll factor in '@unroll(n)'");
        return 0;
      }
      long long factor = strtoll(parser->current_token.value, NULL, 0);
      if (factor < 2 || factor > 16) {
        parser_set_error(parser, "'@unroll' factor must be between 2 and 16");
        return 0;
      }
      out->unroll_factor = (int)factor;
      parser_advance(parser);
      if (!parser_expect(parser, TOKEN_RPAREN)) {
        return 0;
      }
    } else {
      parser_set_error(parser,
                       "Unknown decorator after '@' (expected 'inline', "
                       "'noinline', 'pure', 'noalloc', 'test', 'rule', "
                       "'naked', 'interrupt', 'swappable', 'simd', or "
                       "'unroll')");
      return 0;
    }
  }

  if (out->is_inline && out->is_noinline) {
    parser_set_error(parser,
                     "'@inline' and '@noinline' are mutually exclusive");
    return 0;
  }
  if (out->is_rule &&
      (out->is_inline || out->is_noinline || out->is_pure || out->is_noalloc ||
       out->is_test || out->is_swappable || out->is_naked ||
       out->is_interrupt || out->simd_mode != SIMD_ATTR_NONE)) {
    parser_set_error(parser,
                     "'@rule' stands alone: a rule runs while compiling and "
                     "never becomes code, so no other decorator applies to it");
    return 0;
  }
  /* A swap replaces a function at its call boundary, so the boundary has to
   * still be there. An inlined body has no call to redirect and no single
   * place to redirect it: the copies are spread across every caller. */
  if (out->is_swappable && out->is_inline) {
    parser_set_error(parser,
                     "'@swappable' and '@inline' are mutually exclusive: a "
                     "swap replaces a function at its call boundary, and "
                     "inlining removes that boundary");
    return 0;
  }
  return 1;
}

static ASTNode *parser_parse_comptime_for(Parser *parser, int declarations);
static ASTNode *parser_parse_type_declaration(Parser *parser);
static char *parser_parse_type_annotation(Parser *parser);

/* Function decorators: `@inline` / `@noinline` / `@pure` / `@simd` may
 * prefix a (possibly `export`-qualified) function declaration. Parse the
 * chain, then stamp the flags onto the function it decorates. */
static ASTNode *parser_parse_decorated_declaration(Parser *parser) {
  ParsedDecorators decos;
  if (!parser_parse_decorator_chain(parser, &decos))
    return NULL;
  ASTNode *decl = parser_parse_declaration(parser);
  if (!decl)
    return NULL;
  if (decl->type != AST_FUNCTION_DECLARATION) {
    parser_set_error(parser,
                     "Decorators (@inline/@noinline/@pure/@simd) may only "
                     "precede a function declaration");
    ast_destroy_node(decl);
    return NULL;
  }
  FunctionDeclaration *fd = (FunctionDeclaration *)decl->data;
  if (fd->is_extern) {
    parser_set_error(parser,
                     "Decorators cannot be applied to extern functions");
    ast_destroy_node(decl);
    return NULL;
  }
  if (decos.unroll_factor) {
    parser_set_error(parser,
                     "'@unroll' applies to a loop, not a function");
    ast_destroy_node(decl);
    return NULL;
  }
  fd->is_inline = decos.is_inline;
  fd->is_inline_contract = decos.is_inline_contract;
  fd->is_noinline = decos.is_noinline;
  fd->is_pure = decos.is_pure;
  fd->is_noalloc = decos.is_noalloc;
  fd->is_test = decos.is_test;
  fd->is_swappable = decos.is_swappable;
  fd->is_naked = decos.is_naked;
  fd->is_interrupt = decos.is_interrupt;
  fd->is_rule = decos.is_rule;
  fd->simd_mode = decos.simd_mode;
  if (fd->is_naked && fd->is_interrupt) {
    parser_set_error(parser,
                     "'@naked' and '@interrupt' are mutually exclusive: an "
                     "interrupt handler needs the entry stub '@naked' "
                     "removes");
    ast_destroy_node(decl);
    return NULL;
  }
  if ((fd->is_naked || fd->is_interrupt) && fd->is_inline) {
    parser_set_error(parser,
                     "'@naked' and '@interrupt' functions cannot be inlined");
    ast_destroy_node(decl);
    return NULL;
  }
  return decl;
}

static ASTNode *parser_parse_extern_declaration(Parser *parser) {
  parser_advance(parser); // consume 'extern'
  /* `extern kernel name(params);` declares, host-side, a kernel defined in a
   * separately compiled device module: the signature `dispatch` checks its
   * arguments against. */
  if (parser->current_token.type == TOKEN_FUNCTION ||
      parser->current_token.type == TOKEN_FN ||
      parser->current_token.type == TOKEN_KERNEL) {
    ASTNode *decl = parser_parse_function_declaration(parser);
    if (decl && decl->data) {
      FunctionDeclaration *func_data = (FunctionDeclaration *)decl->data;
      if (func_data->body != NULL) {
        parser_set_error(parser,
                         func_data->is_kernel
                             ? "An 'extern kernel' declaration must not have "
                               "a body; the device module defines it"
                             : "Extern functions must not have a body");
        ast_destroy_node(decl);
        return NULL;
      }
      func_data->is_extern = 1;
    }
    return decl;
  }
  if (parser->current_token.type == TOKEN_VAR) {
    return parser_parse_extern_var_declaration(parser);
  }
  parser_set_error(parser, "Expected 'fn', 'kernel', or 'var' after 'extern'");
  return NULL;
}

static void parser_free_string_array(char **values, size_t count);

static ASTNode *parser_parse_effect_declaration(Parser *parser) {
  SourceLocation location = parser_current_location(parser);
  parser_advance(parser);
  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected an effect name after 'effect'");
    return NULL;
  }
  char *name = strdup(parser->current_token.value);
  parser_advance(parser);
  if (parser->current_token.type != TOKEN_SEMICOLON) {
    parser_set_error(parser, "Expected ';' after the effect name: an effect "
                             "is declared as 'effect Name;'");
    free(name);
    return NULL;
  }
  parser_advance(parser);
  ASTNode *decl = ast_create_effect_declaration(name, location);
  free(name);
  return decl;
}

typedef struct {
  char **names[4];
  size_t counts[4];
} ParsedEffectClauses;

static void parser_free_effect_clauses(ParsedEffectClauses *clauses) {
  for (int c = 0; c < 4; c++) {
    parser_free_string_array(clauses->names[c], clauses->counts[c]);
    clauses->names[c] = NULL;
    clauses->counts[c] = 0;
  }
}

static int parser_effect_clause_index(const Parser *parser) {
  static const char *const words[4] = {"with", "forbids", "requires",
                                       "provides"};
  if (!parser_is_identifier_like(parser->current_token.type) ||
      !parser->current_token.value) {
    return -1;
  }
  for (int c = 0; c < 4; c++) {
    if (strcmp(parser->current_token.value, words[c]) == 0) {
      return c;
    }
  }
  return -1;
}

static int parser_at_effect_name(const Parser *parser) {
  return parser_is_identifier_like(parser->current_token.type) ||
         parser->current_token.type == TOKEN_ASM;
}

static int parser_parse_effect_name_list(Parser *parser, char ***out_names,
                                         size_t *out_count) {
  char **names = NULL;
  size_t count = 0;
  size_t capacity = 0;
  for (;;) {
    if (!parser_at_effect_name(parser)) {
      parser_set_error(parser, "Expected an effect name");
      parser_free_string_array(names, count);
      return 0;
    }
    if (count == capacity) {
      size_t next = capacity ? capacity * 2 : 4;
      char **grown = realloc(names, next * sizeof(char *));
      if (!grown) {
        parser_free_string_array(names, count);
        return 0;
      }
      names = grown;
      capacity = next;
    }
    names[count] = strdup(parser->current_token.type == TOKEN_ASM
                              ? "asm"
                              : parser->current_token.value);
    if (!names[count]) {
      parser_free_string_array(names, count);
      return 0;
    }
    count++;
    parser_advance(parser);
    if (parser->current_token.type != TOKEN_COMMA) {
      break;
    }
    parser_advance(parser);
  }
  *out_names = names;
  *out_count = count;
  return 1;
}

static int parser_parse_effect_clauses(Parser *parser,
                                       ParsedEffectClauses *clauses) {
  static const char *const words[4] = {"with", "forbids", "requires",
                                       "provides"};
  memset(clauses, 0, sizeof(*clauses));
  for (;;) {
    int clause = parser_effect_clause_index(parser);
    if (clause < 0) {
      return 1;
    }
    if (clauses->counts[clause] > 0) {
      char message[128];
      snprintf(message, sizeof(message),
               "'%s' appears twice on this function; list every effect in "
               "the one clause",
               words[clause]);
      parser_set_error(parser, message);
      parser_free_effect_clauses(clauses);
      return 0;
    }
    parser_advance(parser);
    if (!parser_parse_effect_name_list(parser, &clauses->names[clause],
                                       &clauses->counts[clause])) {
      parser_free_effect_clauses(clauses);
      return 0;
    }
  }
}

static int parser_apply_effect_clauses(FunctionDeclaration *decl,
                                       ParsedEffectClauses *clauses) {
  for (int c = 0; c < 4; c++) {
    if (!ast_function_set_effects(decl, c, clauses->names[c],
                                  clauses->counts[c])) {
      return 0;
    }
  }
  parser_free_effect_clauses(clauses);
  return 1;
}

static ASTNode *parser_parse_type_declaration(Parser *parser) {
  SourceLocation location = parser_current_location(parser);
  parser_advance(parser);
  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected a type name after 'type'");
    return NULL;
  }
  char *name = strdup(parser->current_token.value);
  parser_advance(parser);
  if (parser->current_token.type != TOKEN_EQUALS) {
    parser_set_error(parser,
                     "Expected '=' after the type name: a type is declared "
                     "as 'type Name = Base where predicate;'");
    free(name);
    return NULL;
  }
  parser_advance(parser);
  char *base = parser_parse_type_annotation(parser);
  if (!base) {
    free(name);
    return NULL;
  }
  ASTNode *predicate = NULL;
  char *binding = NULL;
  if (parser->current_token.type == TOKEN_WHERE) {
    parser_advance(parser);
    /* `where n: n >= 0` names the value the predicate speaks about. Two
     * tokens settle it: nothing that starts an expression is an identifier
     * followed by a colon, so the default `value` stays unambiguous. */
    if (parser_is_identifier_like(parser->current_token.type) &&
        parser->peek_token.type == TOKEN_COLON) {
      binding = strdup(parser->current_token.value);
      parser_advance(parser);
      parser_advance(parser);
    }
    predicate = parser_parse_expression(parser);
    if (!predicate) {
      free(binding);
      free(name);
      free(base);
      return NULL;
    }
  }
  if (parser->current_token.type != TOKEN_SEMICOLON) {
    parser_set_error(parser, "Expected ';' after the type declaration");
    free(binding);
    free(name);
    free(base);
    if (predicate) {
      ast_destroy_node(predicate);
    }
    return NULL;
  }
  parser_advance(parser);
  ASTNode *decl =
      ast_create_type_declaration(name, base, binding, predicate, location);
  free(binding);
  free(name);
  free(base);
  return decl;
}

static ASTNode *parser_parse_exported_declaration(Parser *parser) {
  parser_advance(parser); // consume 'export'
  ASTNode *decl = NULL;
  if (parser_at_contextual_keyword(parser, "type", TOKEN_IDENTIFIER)) {
    decl = parser_parse_type_declaration(parser);
    if (decl && decl->data) {
      ((TypeDeclaration *)decl->data)->is_exported = 1;
    }
  } else if (parser_at_contextual_keyword(parser, "effect", TOKEN_IDENTIFIER)) {
    decl = parser_parse_effect_declaration(parser);
    if (decl && decl->data) {
      ((EffectDeclaration *)decl->data)->is_exported = 1;
    }
  } else if (parser->current_token.type == TOKEN_FUNCTION ||
      parser->current_token.type == TOKEN_FN) {
    decl = parser_parse_function_declaration(parser);
    if (decl && decl->data) {
      ((FunctionDeclaration *)decl->data)->is_exported = 1;
    }
  } else if (parser->current_token.type == TOKEN_STRUCT) {
    decl = parser_parse_struct_declaration(parser);
    if (decl && decl->data) {
      ((StructDeclaration *)decl->data)->is_exported = 1;
    }
  } else if (parser->current_token.type == TOKEN_ENUM) {
    decl = parser_parse_enum_declaration(parser);
    if (decl && decl->data) {
      ((EnumDeclaration *)decl->data)->is_exported = 1;
    }
  } else if (parser->current_token.type == TOKEN_TRAIT) {
    decl = parser_parse_trait_declaration(parser);
    if (decl && decl->data) {
      ((TraitDeclaration *)decl->data)->is_exported = 1;
    }
  } else if (parser->current_token.type == TOKEN_VAR ||
             parser->current_token.type == TOKEN_CONST) {
    decl = parser_parse_var_declaration(parser);
    if (decl && decl->data) {
      ((VarDeclaration *)decl->data)->is_exported = 1;
    }
  } else if (parser->current_token.type == TOKEN_EXTERN) {
    parser_advance(parser); // consume 'extern'
    if (parser->current_token.type == TOKEN_FUNCTION ||
        parser->current_token.type == TOKEN_FN) {
      decl = parser_parse_function_declaration(parser);
      if (decl && decl->data) {
        FunctionDeclaration *func_data = (FunctionDeclaration *)decl->data;
        if (func_data->body != NULL) {
          parser_set_error(parser, "Extern functions must not have a body");
          ast_destroy_node(decl);
          return NULL;
        }
        func_data->is_extern = 1;
        func_data->is_exported = 1;
      }
    } else if (parser->current_token.type == TOKEN_VAR) {
      decl = parser_parse_extern_var_declaration(parser);
      if (decl && decl->data) {
        ((VarDeclaration *)decl->data)->is_exported = 1;
      }
    } else {
      parser_set_error(parser,
                       "Expected 'fn' or 'var' after 'export extern'");
      return NULL;
    }
  } else if (parser->current_token.type == TOKEN_AT) {
    parser_set_error(parser,
                     "Decorators must precede 'export' (write "
                     "'@inline export fn', not 'export @inline ...')");
    return NULL;
  } else {
    parser_set_error(parser, "Expected 'fn', 'var', 'const', 'struct', "
                             "'enum', 'trait', or 'extern' after 'export'");
    return NULL;
  }
  return decl;
}

static int parser_parse_parameter_list(Parser *parser, char ***out_names,
                                       char ***out_types, size_t *out_count);
static char *parser_parse_type_annotation(Parser *parser);

static void parser_free_rewrite_parts(char **param_names, char **param_types,
                                      size_t param_count, char *return_type,
                                      char *rule_name, ASTNode *from_expr,
                                      ASTNode *to_expr, ASTNode *where_expr) {
  for (size_t i = 0; i < param_count; i++) {
    free(param_names[i]);
    free(param_types[i]);
  }
  free(param_names);
  free(param_types);
  free(return_type);
  free(rule_name);
  ast_destroy_node(from_expr);
  ast_destroy_node(to_expr);
  ast_destroy_node(where_expr);
}

static ASTNode *parser_make_rewrite_side(const char *prefix,
                                         const char *rule_name,
                                         char **param_names,
                                         char **param_types,
                                         size_t param_count,
                                         const char *return_type,
                                         ASTNode *expr, SourceLocation location,
                                         int role) {
  ASTNode *return_stmt = ast_create_node(AST_RETURN_STATEMENT, expr->location);
  if (!return_stmt) {
    return NULL;
  }
  ReturnStatement *ret_data = malloc(sizeof(ReturnStatement));
  ASTNode **values = malloc(sizeof(ASTNode *));
  if (!ret_data || !values) {
    free(ret_data);
    free(values);
    free(return_stmt);
    return NULL;
  }
  values[0] = expr;
  ret_data->value = expr;
  ret_data->values = values;
  ret_data->value_count = 1;
  return_stmt->data = ret_data;
  ast_add_child(return_stmt, expr);

  ASTNode *body = ast_create_program();
  if (!body) {
    ast_destroy_node(return_stmt);
    return NULL;
  }
  Program *body_data = (Program *)body->data;
  body_data->declarations = malloc(sizeof(ASTNode *));
  if (!body_data->declarations) {
    ast_destroy_node(return_stmt);
    ast_destroy_node(body);
    return NULL;
  }
  body_data->declarations[0] = return_stmt;
  body_data->declaration_count = 1;
  ast_add_child(body, return_stmt);

  size_t name_length = strlen(prefix) + strlen(rule_name) + 1;
  char *name = malloc(name_length);
  if (!name) {
    ast_destroy_node(body);
    return NULL;
  }
  snprintf(name, name_length, "%s%s", prefix, rule_name);
  ASTNode *decl = ast_create_function_declaration(
      name, param_names, param_types, param_count, return_type, body,
      location);
  free(name);
  if (!decl) {
    ast_destroy_node(body);
    return NULL;
  }
  ((FunctionDeclaration *)decl->data)->rewrite_role = role;
  return decl;
}

static ASTNode *parser_parse_rewrite_declaration(Parser *parser) {
  SourceLocation location = parser_current_location(parser);
  parser_advance(parser);

  if (parser->comptime_depth > 0) {
    parser_set_error(parser,
                     "A 'rewrite' rule cannot be generated by 'comptime for'");
    return NULL;
  }
  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected a rule name after 'rewrite'");
    return NULL;
  }
  char *rule_name = strdup(parser->current_token.value);
  parser_advance(parser);
  if (!rule_name) {
    return NULL;
  }

  char **param_names = NULL;
  char **param_types = NULL;
  size_t param_count = 0;
  char *return_type = NULL;
  ASTNode *from_expr = NULL;
  ASTNode *to_expr = NULL;
  ASTNode *where_expr = NULL;

  if (!parser_expect(parser, TOKEN_LPAREN) ||
      !parser_parse_parameter_list(parser, &param_names, &param_types,
                                   &param_count) ||
      !parser_expect(parser, TOKEN_RPAREN)) {
    parser_free_rewrite_parts(param_names, param_types, param_count, NULL,
                              rule_name, NULL, NULL, NULL);
    return NULL;
  }
  if (param_count == 0) {
    parser_set_error(parser,
                     "A 'rewrite' rule needs at least one parameter: the "
                     "values its 'from' side matches");
    parser_free_rewrite_parts(param_names, param_types, param_count, NULL,
                              rule_name, NULL, NULL, NULL);
    return NULL;
  }
  if (parser->current_token.type != TOKEN_ARROW) {
    parser_set_error(parser,
                     "Expected '-> type' after the rule's parameters: the "
                     "type both sides of the rule compute");
    parser_free_rewrite_parts(param_names, param_types, param_count, NULL,
                              rule_name, NULL, NULL, NULL);
    return NULL;
  }
  parser_advance(parser);
  return_type = parser_parse_type_annotation(parser);
  if (!return_type) {
    if (!parser->has_error) {
      parser_set_error(parser, "Expected the rule's result type after '->'");
    }
    parser_free_rewrite_parts(param_names, param_types, param_count, NULL,
                              rule_name, NULL, NULL, NULL);
    return NULL;
  }
  if (!parser_expect(parser, TOKEN_LBRACE)) {
    parser_free_rewrite_parts(param_names, param_types, param_count,
                              return_type, rule_name, NULL, NULL, NULL);
    return NULL;
  }

  while (parser->current_token.type != TOKEN_RBRACE &&
         parser->current_token.type != TOKEN_EOF) {
    if (parser->current_token.type == TOKEN_NEWLINE ||
        parser->current_token.type == TOKEN_SEMICOLON) {
      parser_advance(parser);
      continue;
    }
    ASTNode **slot = NULL;
    const char *clause = NULL;
    if (parser->current_token.type == TOKEN_WHERE) {
      slot = &where_expr;
      clause = "where";
    } else if (parser_is_identifier_like(parser->current_token.type) &&
               parser->current_token.value &&
               strcmp(parser->current_token.value, "from") == 0) {
      slot = &from_expr;
      clause = "from";
    } else if (parser_is_identifier_like(parser->current_token.type) &&
               parser->current_token.value &&
               strcmp(parser->current_token.value, "to") == 0) {
      slot = &to_expr;
      clause = "to";
    } else {
      parser_set_error(parser,
                       "Expected 'from', 'to' or 'where' inside a 'rewrite' "
                       "rule");
      parser_free_rewrite_parts(param_names, param_types, param_count,
                                return_type, rule_name, from_expr, to_expr,
                                where_expr);
      return NULL;
    }
    if (*slot) {
      char message[128];
      snprintf(message, sizeof(message),
               "A 'rewrite' rule has one '%s' clause", clause);
      parser_set_error(parser, message);
      parser_free_rewrite_parts(param_names, param_types, param_count,
                                return_type, rule_name, from_expr, to_expr,
                                where_expr);
      return NULL;
    }
    parser_advance(parser);
    *slot = parser_parse_expression(parser);
    if (!*slot) {
      if (!parser->has_error) {
        char message[128];
        snprintf(message, sizeof(message),
                 "Expected an expression after '%s'", clause);
        parser_set_error(parser, message);
      }
      parser_free_rewrite_parts(param_names, param_types, param_count,
                                return_type, rule_name, from_expr, to_expr,
                                where_expr);
      return NULL;
    }
    if (parser->current_token.type != TOKEN_SEMICOLON &&
        parser->current_token.type != TOKEN_NEWLINE &&
        parser->current_token.type != TOKEN_RBRACE) {
      parser_set_error(parser,
                       "Expected ';' after the clause's expression");
      parser_free_rewrite_parts(param_names, param_types, param_count,
                                return_type, rule_name, from_expr, to_expr,
                                where_expr);
      return NULL;
    }
  }
  if (!parser_expect(parser, TOKEN_RBRACE)) {
    parser_free_rewrite_parts(param_names, param_types, param_count,
                              return_type, rule_name, from_expr, to_expr,
                              where_expr);
    return NULL;
  }
  if (!from_expr || !to_expr) {
    parser_set_error(parser,
                     !from_expr ? "A 'rewrite' rule needs a 'from' clause: "
                                  "the expression it matches"
                                : "A 'rewrite' rule needs a 'to' clause: the "
                                  "expression it replaces the match with");
    parser_free_rewrite_parts(param_names, param_types, param_count,
                              return_type, rule_name, from_expr, to_expr,
                              where_expr);
    return NULL;
  }

  ASTNode *from_decl = parser_make_rewrite_side(
      IR_REWRITE_FROM_PREFIX, rule_name, param_names, param_types,
      param_count, return_type, from_expr, location, IR_REWRITE_ROLE_FROM);
  ASTNode *to_decl = from_decl ? parser_make_rewrite_side(
      IR_REWRITE_TO_PREFIX, rule_name, param_names, param_types,
      param_count, return_type, to_expr, location, IR_REWRITE_ROLE_TO)
                               : NULL;
  ASTNode *where_decl = NULL;
  if (to_decl && where_expr) {
    where_decl = parser_make_rewrite_side(
        IR_REWRITE_WHERE_PREFIX, rule_name, param_names, param_types,
        param_count, "bool", where_expr, location, IR_REWRITE_ROLE_WHERE);
  }
  int built = from_decl && to_decl && (where_decl || !where_expr);
  parser_free_rewrite_parts(param_names, param_types, param_count,
                            return_type, rule_name,
                            from_decl ? NULL : from_expr,
                            to_decl ? NULL : to_expr,
                            (where_decl || !where_expr) ? NULL : where_expr);
  if (!built) {
    ast_destroy_node(from_decl);
    ast_destroy_node(to_decl);
    ast_destroy_node(where_decl);
    parser_set_error(parser, "Out of memory while building a 'rewrite' rule");
    return NULL;
  }
  parser->extra_declarations[0] = to_decl;
  parser->extra_declaration_count = 1;
  if (where_decl) {
    parser->extra_declarations[1] = where_decl;
    parser->extra_declaration_count = 2;
  }
  return from_decl;
}

ASTNode *parser_parse_declaration(Parser *parser) {
  if (!parser)
    return NULL;

  /* Contextual `comptime for` in declaration position: the body holds
   * declarations, and its expansions are spliced into the enclosing module. */
  if (parser_at_contextual_keyword(parser, "comptime", TOKEN_FOR)) {
    return parser_parse_comptime_for(parser, 1);
  }
  if (parser_at_contextual_keyword(parser, "rewrite", TOKEN_IDENTIFIER)) {
    return parser_parse_rewrite_declaration(parser);
  }
  if (parser_at_contextual_keyword(parser, "type", TOKEN_IDENTIFIER)) {
    return parser_parse_type_declaration(parser);
  }
  if (parser_at_contextual_keyword(parser, "effect", TOKEN_IDENTIFIER)) {
    return parser_parse_effect_declaration(parser);
  }

  if (parser->current_token.type == TOKEN_AT) {
    return parser_parse_decorated_declaration(parser);
  }

  switch (parser->current_token.type) {
  case TOKEN_IMPORT:
    return parser_parse_import_declaration(parser);
  case TOKEN_EXTERN:
    return parser_parse_extern_declaration(parser);
  case TOKEN_EXPORT:
    return parser_parse_exported_declaration(parser);
  case TOKEN_VAR:
  case TOKEN_CONST:
  case TOKEN_WORKGROUP:
  case TOKEN_PRIVATE:
    return parser_parse_var_declaration(parser);
  case TOKEN_BARRIER:
    return parser_parse_barrier_statement(parser);
  case TOKEN_DEFER:
    return parser_parse_defer_statement(parser);
  case TOKEN_ERRDEFER:
    return parser_parse_errdefer_statement(parser);
  case TOKEN_FUNCTION:
  case TOKEN_FN:
  case TOKEN_KERNEL:
    return parser_parse_function_declaration(parser);
  case TOKEN_STRUCT:
    return parser_parse_struct_declaration(parser);
  case TOKEN_ENUM:
    return parser_parse_enum_declaration(parser);
  case TOKEN_TRAIT:
    return parser_parse_trait_declaration(parser);
  case TOKEN_IMPL:
    return parser_parse_impl_declaration(parser);
  default:
    /* Try to parse as a statement instead. */
    return parser_parse_statement(parser);
  }
}

static int parser_is_assignment_target(ASTNode *target) {
  if (!target) {
    return 0;
  }

  if (target->type == AST_IDENTIFIER || target->type == AST_MEMBER_ACCESS ||
      target->type == AST_INDEX_EXPRESSION) {
    return 1;
  }

  if (target->type == AST_UNARY_EXPRESSION) {
    UnaryExpression *unary = (UnaryExpression *)target->data;
    return unary && unary->operator && strcmp(unary->operator, "*") == 0;
  }

  return 0;
}

static const char *parser_compound_assign_op(TokenType type) {
  switch (type) {
  case TOKEN_PLUS_EQUALS:    return "+";
  case TOKEN_MINUS_EQUALS:   return "-";
  case TOKEN_STAR_EQUALS:    return "*";
  case TOKEN_SLASH_EQUALS:   return "/";
  case TOKEN_PERCENT_EQUALS: return "%";
  case TOKEN_AMP_EQUALS:     return "&";
  case TOKEN_PIPE_EQUALS:    return "|";
  case TOKEN_CARET_EQUALS:   return "^";
  case TOKEN_LSHIFT_EQUALS:  return "<<";
  case TOKEN_RSHIFT_EQUALS:  return ">>";
  default:                   return NULL;
  }
}

/* `i++` and `i--` are statements, exactly like `i += 1` and `i -= 1`. They
 * carry no value, so there is no order-of-evaluation question to answer and no
 * prefix/postfix distinction to observe: both spellings mean the same step. */
static const char *parser_increment_op(TokenType type) {
  switch (type) {
  case TOKEN_PLUS_PLUS:   return "+";
  case TOKEN_MINUS_MINUS: return "-";
  default:                return NULL;
  }
}

int parser_is_assignment_token(TokenType type) {
  return type == TOKEN_EQUALS || parser_compound_assign_op(type) != NULL ||
         parser_increment_op(type) != NULL;
}

static ASTNode *parser_parse_assignment_from_target(Parser *parser,
                                                    ASTNode *target) {
  if (!parser || !target) {
    return NULL;
  }

  if (!parser_is_assignment_target(target)) {
    parser_set_error(parser, "Invalid assignment target");
    ast_destroy_node(target);
    return NULL;
  }

  TokenType assign_token = parser->current_token.type;
  const char *compound_op = parser_compound_assign_op(assign_token);
  const char *increment_op = parser_increment_op(assign_token);
  ASTNode *value = NULL;

  if (increment_op) {
    compound_op = increment_op;
  }

  parser_advance(parser); // consume '=', a compound operator, or '++' / '--'

  if (increment_op) {
    /* The step is the only operand `++` can have, so it is synthesized here
     * rather than parsed. */
    value = ast_create_number_literal(1, target->location, 10);
  } else {
    value = parser_parse_expression(parser);
  }
  if (!value) {
    ast_destroy_node(target);
    return NULL;
  }

  // Desugar `target OP= value` into `target = target OP value`.
  if (compound_op) {
    ASTNode *target_clone = ast_clone_node(target);
    if (!target_clone) {
      ast_destroy_node(target);
      ast_destroy_node(value);
      parser_set_error(parser, "Out of memory cloning compound assignment target");
      return NULL;
    }
    ASTNode *combined = ast_create_binary_expression(target_clone, compound_op,
                                                     value, target->location);
    if (!combined) {
      ast_destroy_node(target);
      ast_destroy_node(target_clone);
      ast_destroy_node(value);
      return NULL;
    }
    value = combined;
  }

  if (target->type == AST_IDENTIFIER) {
    Identifier *id = (Identifier *)target->data;
    if (!id || !id->name) {
      ast_destroy_node(target);
      ast_destroy_node(value);
      parser_set_error(parser, "Invalid assignment target");
      return NULL;
    }

    ASTNode *assign = ast_create_assignment(id->name, value, target->location);
    ast_destroy_node(target);
    return assign;
  }

  return ast_create_field_assignment(target, value, target->location);
}

/* `++target` and `--target` in statement position. The step is identical to the
 * postfix spelling above; only the order the two tokens are read in differs. */
static ASTNode *parser_parse_prefix_increment(Parser *parser) {
  if (!parser) {
    return NULL;
  }

  const char *op = parser_increment_op(parser->current_token.type);
  if (!op) {
    return NULL;
  }

  parser_advance(parser); // consume '++' or '--'

  ASTNode *target = parser_parse_expression(parser);
  if (!target) {
    return NULL;
  }
  if (!parser_is_assignment_target(target)) {
    parser_set_error(parser, "Invalid assignment target");
    ast_destroy_node(target);
    return NULL;
  }

  ASTNode *target_clone = ast_clone_node(target);
  if (!target_clone) {
    parser_set_error(parser, "Out of memory cloning increment target");
    ast_destroy_node(target);
    return NULL;
  }

  ASTNode *step = ast_create_number_literal(1, target->location, 10);
  if (!step) {
    ast_destroy_node(target);
    ast_destroy_node(target_clone);
    return NULL;
  }

  ASTNode *value =
      ast_create_binary_expression(target_clone, op, step, target->location);
  if (!value) {
    ast_destroy_node(target);
    ast_destroy_node(target_clone);
    ast_destroy_node(step);
    return NULL;
  }

  if (target->type == AST_IDENTIFIER) {
    Identifier *id = (Identifier *)target->data;
    if (!id || !id->name) {
      ast_destroy_node(target);
      ast_destroy_node(value);
      parser_set_error(parser, "Invalid assignment target");
      return NULL;
    }

    ASTNode *assign = ast_create_assignment(id->name, value, target->location);
    ast_destroy_node(target);
    return assign;
  }

  return ast_create_field_assignment(target, value, target->location);
}

static ASTNode *parser_parse_dispatch_statement(Parser *parser);

static ASTNode *parser_parse_parenthesized_assignment(Parser *parser) {
  if (!parser || parser->current_token.type != TOKEN_LPAREN) {
    return NULL;
  }

  SourceLocation location = parser_current_location(parser);
  parser_advance(parser);
  size_t capacity = 4;
  size_t target_count = 0;
  ASTNode **targets = calloc(capacity, sizeof(ASTNode *));
  if (!targets) {
    return NULL;
  }

  ASTNode *first = parser_parse_expression(parser);
  if (!first) {
    free(targets);
    return NULL;
  }
  targets[target_count++] = first;
  if (parser->current_token.type != TOKEN_COMMA) {
    if (!parser_expect(parser, TOKEN_RPAREN)) {
      ast_destroy_node(first);
      free(targets);
      return NULL;
    }
    if (parser_is_assignment_token(parser->current_token.type)) {
      ASTNode *assignment = parser_parse_assignment_from_target(parser, first);
      free(targets);
      return assignment;
    }
    free(targets);
    return first;
  }

  while (parser->current_token.type == TOKEN_COMMA) {
    parser_advance(parser);
    ASTNode *target = parser_parse_expression(parser);
    if (!target) {
      for (size_t i = 0; i < target_count; i++) {
        ast_destroy_node(targets[i]);
      }
      free(targets);
      return NULL;
    }
    if (target_count == capacity) {
      capacity *= 2;
      ASTNode **grown = realloc(targets, capacity * sizeof(ASTNode *));
      if (!grown) {
        ast_destroy_node(target);
        for (size_t i = 0; i < target_count; i++) {
          ast_destroy_node(targets[i]);
        }
        free(targets);
        return NULL;
      }
      targets = grown;
    }
    targets[target_count++] = target;
  }

  if (!parser_expect(parser, TOKEN_RPAREN)) {
    for (size_t i = 0; i < target_count; i++) {
      ast_destroy_node(targets[i]);
    }
    free(targets);
    return NULL;
  }
  if (!parser_is_assignment_token(parser->current_token.type)) {
    parser_set_error(parser, "A multiple return list must assign to targets");
    for (size_t i = 0; i < target_count; i++) {
      ast_destroy_node(targets[i]);
    }
    free(targets);
    return NULL;
  }
  if (parser->current_token.type != TOKEN_EQUALS) {
    parser_set_error(parser, "Multiple return assignment only supports '='");
    for (size_t i = 0; i < target_count; i++) {
      ast_destroy_node(targets[i]);
    }
    free(targets);
    return NULL;
  }
  parser_advance(parser);
  ASTNode *value = parser_parse_expression(parser);
  if (!value) {
    for (size_t i = 0; i < target_count; i++) {
      ast_destroy_node(targets[i]);
    }
    free(targets);
    return NULL;
  }
  ASTNode *assignment = ast_create_multi_assignment(targets, target_count,
                                                     value, location);
  if (!assignment) {
    for (size_t i = 0; i < target_count; i++) {
      ast_destroy_node(targets[i]);
    }
    free(targets);
    ast_destroy_node(value);
  }
  return assignment;
}

/* Enough of the parser's position to rewind a speculative parse. */
typedef struct {
  size_t lexer_position;
  size_t lexer_line;
  size_t lexer_column;
  size_t lexer_continuation_depth;
  Token current_token;
  Token peek_token;
  int has_error;
  int had_error;
  size_t error_count;
  char *error_message;
  ErrorReporter *error_reporter;
} ParserSavedState;

static ParserSavedState parser_save_state(Parser *parser);
static void parser_restore_state(Parser *parser,
                                 const ParserSavedState *state);
static void parser_discard_saved_state(ParserSavedState *state);

/* True where `ident(` starts a composed declaration name rather than naming a
 * function the programmer wrote. Only inside a `comptime for`, so a program
 * with its own `ident` is untouched. */
static int parser_at_composed_name(Parser *parser) {
  return parser->comptime_depth > 0 &&
         parser_is_identifier_like(parser->current_token.type) &&
         parser->current_token.value &&
         strcmp(parser->current_token.value, "ident") == 0 &&
         parser->peek_token.type == TOKEN_LPAREN;
}

/* `ident("prefix", f.name)` where a declaration's name goes.
 *
 * A metaprogram that generates one declaration per field needs each of them to
 * have a different name, and the only compile-time strings in the language are
 * the ones reflection answers with. So the name is composed the way `typeof`
 * and `offsetof` were built: an identifier and call syntax, adding no
 * punctuation the lexer did not already read.
 *
 * The parts stay unevaluated here. The expander joins them once per iteration,
 * which is the only point at which the binding has a value. */
static ASTNode *parser_parse_composed_name(Parser *parser) {
  SourceLocation location = parser_current_location(parser);
  parser_advance(parser); /* consume the contextual `ident` */
  if (!parser_expect(parser, TOKEN_LPAREN)) {
    return NULL;
  }

  ASTNode **parts = NULL;
  size_t count = 0;
  while (parser->current_token.type != TOKEN_RPAREN &&
         parser->current_token.type != TOKEN_EOF && !parser->has_error) {
    if (count > 0 && !parser_expect(parser, TOKEN_COMMA)) {
      break;
    }
    ASTNode *part = parser_parse_expression(parser);
    if (!part) {
      if (!parser->has_error) {
        parser_set_error(parser,
                         "Expected a compile-time string in 'ident(...)'");
      }
      break;
    }
    ASTNode **grown = realloc(parts, (count + 1) * sizeof(ASTNode *));
    if (!grown) {
      ast_destroy_node(part);
      break;
    }
    parts = grown;
    parts[count++] = part;
  }

  if (parser->has_error || !parser_expect(parser, TOKEN_RPAREN)) {
    for (size_t i = 0; i < count; i++) {
      ast_destroy_node(parts[i]);
    }
    free(parts);
    return NULL;
  }
  if (count == 0) {
    parser_set_error(parser,
                     "'ident()' composes a name from compile-time strings and "
                     "needs at least one");
    free(parts);
    return NULL;
  }

  ASTNode *node = ast_create_call_expression("ident", parts, count, location);
  free(parts);
  if (!node) {
    return NULL;
  }
  return node;
}

/* Read a declaration's name: either an identifier, or the `ident(...)` form
 * that composes one. Exactly one of `*out_name` and `*out_composed` is set. */
static int parser_parse_declaration_name(Parser *parser, const char *expected,
                                         char **out_name,
                                         ASTNode **out_composed) {
  *out_name = NULL;
  *out_composed = NULL;
  /* Whatever the last declaration parse abandoned. */
  ast_destroy_node(parser->pending_composed_name);
  parser->pending_composed_name = NULL;

  /* `ident("...")` reads as an attempt to compose a name wherever it appears:
   * no parameter list starts with a string. Saying so beats letting it fail
   * further along as a malformed parameter, and a function the programmer
   * really did call `ident` is untouched, because its parameters are named. */
  if (parser->comptime_depth == 0 &&
      parser_is_identifier_like(parser->current_token.type) &&
      parser->current_token.value &&
      strcmp(parser->current_token.value, "ident") == 0 &&
      parser->peek_token.type == TOKEN_LPAREN) {
    ParserSavedState saved = parser_save_state(parser);
    parser_advance(parser); /* `ident` */
    parser_advance(parser); /* '(' */
    int composes = parser->current_token.type == TOKEN_STRING;
    parser_restore_state(parser, &saved);
    parser_discard_saved_state(&saved);
    if (composes) {
      parser_set_error(parser,
                       "'ident(...)' composes a name for generated code; it "
                       "needs a 'comptime for' around it to generate any");
      return 0;
    }
  }

  if (parser_at_composed_name(parser)) {
    *out_composed = parser_parse_composed_name(parser);
    if (!*out_composed) {
      return 0;
    }
    parser->pending_composed_name = *out_composed;
    /* A placeholder, so anything that reaches for a name before the expander
     * has run reads as unresolved rather than as a crash. */
    *out_name = strdup("<ident>");
    return *out_name != NULL;
  }

  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, expected);
    return 0;
  }
  *out_name = strdup(parser->current_token.value);
  parser_advance(parser);
  return *out_name != NULL;
}

/* A `comptime for` body at module scope holds declarations, where one inside a
 * function holds statements. The directive is the same either way; what
 * differs is the list its expansions are spliced into. */
static ASTNode *parser_parse_declaration_block(Parser *parser) {
  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, TOKEN_LBRACE)) {
    return NULL;
  }

  ASTNode *block = ast_create_program();
  if (!block) {
    return NULL;
  }
  block->location = location;
  Program *data = (Program *)block->data;

  while (parser->current_token.type != TOKEN_RBRACE &&
         parser->current_token.type != TOKEN_EOF && !parser->has_error) {
    if (parser->current_token.type == TOKEN_NEWLINE ||
        parser->current_token.type == TOKEN_SEMICOLON) {
      parser_advance(parser);
      continue;
    }
    ASTNode *declaration = parser_parse_declaration(parser);
    if (!declaration) {
      if (!parser->has_error) {
        parser_set_error(parser,
                         "Expected a declaration in this 'comptime for' body");
      }
      break;
    }
    ASTNode **grown =
        realloc(data->declarations,
                (data->declaration_count + 1) * sizeof(ASTNode *));
    if (!grown) {
      ast_destroy_node(declaration);
      break;
    }
    data->declarations = grown;
    data->declarations[data->declaration_count++] = declaration;
    ast_add_child(block, declaration);
  }

  if (parser->has_error || !parser_expect(parser, TOKEN_RBRACE)) {
    ast_destroy_node(block);
    return NULL;
  }
  return block;
}

/* `comptime for <binding> in <sequence> { <body> }`.
 *
 * `comptime` is contextual, the same way `in` is: no token and no keyword is
 * reserved for it, so a program that already uses `comptime` as a name keeps
 * working. Only `comptime` immediately followed by `for` starts one of these.
 *
 * The binding carries no type annotation because it is bound to a `Field` by
 * the expander, which is the one type it can ever have. The body is always
 * braced -- an expansion is a block, and a block is what gives each iteration
 * its own scope. */
static ASTNode *parser_parse_comptime_for(Parser *parser, int declarations) {
  SourceLocation location = parser_current_location(parser);
  parser_advance(parser); // consume the contextual `comptime`
  if (!parser_expect(parser, TOKEN_FOR)) {
    return NULL;
  }

  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser,
                     "Expected a binding name after 'comptime for'");
    return NULL;
  }
  char *binding_name = strdup(parser->current_token.value);
  if (!binding_name) {
    return NULL;
  }
  parser_advance(parser);

  if (parser->current_token.type == TOKEN_COLON) {
    parser_set_error(parser,
                     "A 'comptime for' binding is always a 'Field'; drop the "
                     "type annotation");
    free(binding_name);
    return NULL;
  }

  // Contextual `in`, the same spelling the range-based `for` uses.
  if (!parser_is_identifier_like(parser->current_token.type) ||
      strcmp(parser->current_token.value, "in") != 0) {
    parser_set_error(parser,
                     "Expected 'in' after the 'comptime for' binding");
    free(binding_name);
    return NULL;
  }
  parser_advance(parser);

  ASTNode *sequence = parser_parse_expression(parser);
  if (!sequence) {
    if (!parser->has_error) {
      parser_set_error(parser,
                       "Expected a compile-time sequence after 'in', for "
                       "example 'typeof(T).fields'");
    }
    free(binding_name);
    return NULL;
  }

  if (parser->current_token.type != TOKEN_LBRACE) {
    parser_set_error(parser, "Expected '{' to open the 'comptime for' body");
    free(binding_name);
    ast_destroy_node(sequence);
    return NULL;
  }

  parser->comptime_depth++;
  ASTNode *body = declarations ? parser_parse_declaration_block(parser)
                               : parser_parse_block(parser);
  parser->comptime_depth--;
  if (!body) {
    free(binding_name);
    ast_destroy_node(sequence);
    return NULL;
  }

  ASTNode *node =
      ast_create_comptime_for(binding_name, sequence, body, location);
  free(binding_name);
  if (!node) {
    ast_destroy_node(sequence);
    ast_destroy_node(body);
    return NULL;
  }
  return node;
}

/* Words that declare something in C, Rust, Go, or Python and nothing in
   Mettle. Naming the Mettle spelling once beats letting the reader work back
   from a run of grammar complaints. */
static const struct {
  const char *word;
  const char *message;
  const char *help;
} kForeignDeclarators[] = {
    {"let", "'let' declares nothing in Mettle",
     "locals start with 'var': var x: int64 = 0;"},
    {"mut", "'mut' declares nothing in Mettle",
     "every 'var' is already mutable: var x: int64 = 0;"},
    {"auto", "Mettle infers no types, so 'auto' declares nothing",
     "name the type: var x: int64 = 0;"},
    {"def", "'def' declares nothing in Mettle",
     "functions start with 'fn': fn name() -> int64 { ... }"},
    {"func", "'func' declares nothing in Mettle",
     "functions start with 'fn': fn name() -> int64 { ... }"},
    {"int", "'int' is not a Mettle type",
     "the integer types carry their width: int8, int16, int32, int64"},
    {"uint", "'uint' is not a Mettle type",
     "the unsigned types carry their width: uint8, uint16, uint32, uint64"},
    {"unsigned", "'unsigned' is not a Mettle type",
     "the unsigned types carry their width: uint8, uint16, uint32, uint64"},
    {"signed", "'signed' is not a Mettle type",
     "the signed types carry their width: int8, int16, int32, int64"},
    {"long", "'long' is not a Mettle type", "write 'int64'"},
    {"short", "'short' is not a Mettle type", "write 'int16'"},
    {"char", "'char' is not a Mettle type",
     "a byte is 'uint8'; text is 'string'"},
    {"double", "'double' is not a Mettle type", "write 'float64'"},
    {"float", "'float' is not a Mettle type",
     "the float types carry their width: float32, float64"},
    {"void", "'void' is not a Mettle type",
     "a function that returns nothing leaves the '-> type' off"},
};

/* Two names in a row start no Mettle statement, so the reader has written a
   declaration in another language's grammar. Say so and stop, rather than
   letting the expression parser complain about the second name. Returns 1
   when it reported. */
static int parser_reject_foreign_declaration(Parser *parser) {
  if (parser->current_token.type != TOKEN_IDENTIFIER &&
      !(parser->current_token.type >= TOKEN_MOV &&
        parser->current_token.type <= TOKEN_SYSCALL))
    return 0;
  if (parser->peek_token.type != TOKEN_IDENTIFIER &&
      !parser_is_type_keyword(parser->peek_token.type))
    return 0;
  const char *word = parser->current_token.value;
  if (!word)
    return 0;

  for (size_t i = 0; i < sizeof(kForeignDeclarators) / sizeof(*kForeignDeclarators);
       i++) {
    if (strcmp(word, kForeignDeclarators[i].word) != 0)
      continue;
    parser_set_error_with_suggestion(parser, kForeignDeclarators[i].message,
                                     kForeignDeclarators[i].help);
    if (parser->error_reporter)
      error_reporter_set_last_label(parser->error_reporter,
                                    "not a Mettle declaration");
    return 1;
  }

  char message[PARSER_ERROR_BUF_SIZE];
  char help[PARSER_ERROR_BUF_SIZE];
  snprintf(message, sizeof(message),
           "Expected an operator or the end of the statement after '%s', found "
           "the name '%s'",
           word, parser->peek_token.value ? parser->peek_token.value : "it");
  snprintf(help, sizeof(help),
           "a declaration names the type after the variable: var %s: %s = ...;",
           parser->peek_token.value ? parser->peek_token.value : "name", word);
  parser_set_error_with_suggestion(parser, message, help);
  return 1;
}

ASTNode *parser_parse_statement(Parser *parser) {
  if (!parser)
    return NULL;

  // Contextual `comptime for`: only this exact pair starts a compile-time
  // loop, so `comptime` stays available as an ordinary identifier.
  if (parser_at_contextual_keyword(parser, "comptime", TOKEN_FOR)) {
    return parser_parse_comptime_for(parser, 0);
  }

  // Contextual `quiesce;`: the swap point. Contextual the same way `comptime`
  // is, so a program already using `quiesce` as a name keeps working. The
  // semicolon is what tells the marker from a variable of that name, and the
  // only thing it takes away is a bare `quiesce;` expression statement, which
  // reads a value and discards it.
  if (parser_at_contextual_keyword(parser, "quiesce", TOKEN_SEMICOLON)) {
    SourceLocation location = parser_current_location(parser);
    parser_advance(parser); // consume the contextual `quiesce`
    parser_advance(parser); // consume ';'
    return ast_create_quiesce_statement(location);
  }

  /* Contextual `fallthrough;`: continue into the next case of a switch. A case
   * ends where the next one begins, so this is what asks for the other
   * behaviour, and it is contextual for the same reason `quiesce` is. */
  if (parser_at_contextual_keyword(parser, "fallthrough", TOKEN_SEMICOLON)) {
    SourceLocation location = parser_current_location(parser);
    parser_advance(parser);
    parser_advance(parser);
    return ast_create_fallthrough_statement(location);
  }

  // Vectorization attribute on a loop: `@simd` / `@simd!`.
  //   @simd  for i in 0..n { ... }   -> best-effort hint (warn if not vectorized)
  //   @simd! for i in 0..n { ... }   -> hard contract (compile error otherwise)
  // The attribute may sit in front of a label too: `@simd outer: for ...`.
  // Only `@simd` is meaningful on a loop; the other decorators are function-only.
  if (parser->current_token.type == TOKEN_AT) {
    ParsedDecorators decos;
    if (!parser_parse_decorator_chain(parser, &decos))
      return NULL;
    if (decos.is_inline || decos.is_noinline || decos.is_pure ||
        decos.is_noalloc || decos.is_swappable || decos.is_rule) {
      parser_set_error(parser,
                       "'@inline', '@noinline', '@pure', '@noalloc', "
                       "'@swappable', and '@rule' apply to a function, not a "
                       "loop");
      return NULL;
    }
    if (decos.simd_mode == SIMD_ATTR_NONE && !decos.unroll_factor) {
      parser_set_error(
          parser, "Expected a '@simd' or '@unroll' decorator before a loop");
      return NULL;
    }

    ASTNode *loop = parser_parse_statement(parser);
    if (!loop)
      return NULL;
    if (loop->type == AST_FOR_STATEMENT) {
      ((ForStatement *)loop->data)->simd_mode = decos.simd_mode;
      ((ForStatement *)loop->data)->unroll_factor = decos.unroll_factor;
    } else if (loop->type == AST_WHILE_STATEMENT) {
      ((WhileStatement *)loop->data)->simd_mode = decos.simd_mode;
      ((WhileStatement *)loop->data)->unroll_factor = decos.unroll_factor;
    } else {
      parser_set_error(
          parser,
          "'@simd' / '@unroll' must be applied to a 'for' or 'while' loop");
      ast_destroy_node(loop);
      return NULL;
    }
    return loop;
  }

  // Labeled loop: IDENT ':' (while | for)
  if (parser->current_token.type == TOKEN_IDENTIFIER &&
      parser->peek_token.type == TOKEN_COLON) {
    Token after_colon = lexer_peek_token(parser->lexer);
    if (after_colon.type == TOKEN_WHILE || after_colon.type == TOKEN_FOR) {
      char *label = strdup(parser->current_token.value);
      parser_advance(parser); // consume IDENT
      parser_advance(parser); // consume ':'

      ASTNode *loop = NULL;
      if (parser->current_token.type == TOKEN_WHILE) {
        loop = parser_parse_while_statement(parser);
        if (loop && loop->data) {
          WhileStatement *w = (WhileStatement *)loop->data;
          w->label = label;
          label = NULL;
        }
      } else if (parser->current_token.type == TOKEN_FOR) {
        loop = parser_parse_for_statement(parser);
        if (loop && loop->data) {
          ForStatement *f = (ForStatement *)loop->data;
          f->label = label;
          label = NULL;
        }
      }

      free(label);
      return loop;
    }
  }

  switch (parser->current_token.type) {
  case TOKEN_PLUS_PLUS:
  case TOKEN_MINUS_MINUS:
    return parser_parse_prefix_increment(parser);
  case TOKEN_EXTERN:
    return parser_parse_declaration(parser);
  case TOKEN_VAR:
  case TOKEN_CONST:
  case TOKEN_WORKGROUP:
  case TOKEN_PRIVATE:
    return parser_parse_var_declaration(parser);
  case TOKEN_BARRIER:
    return parser_parse_barrier_statement(parser);
  case TOKEN_RETURN:
    return parser_parse_return_statement(parser);
  case TOKEN_IF:
    return parser_parse_if_statement(parser);
  case TOKEN_WHILE:
    return parser_parse_while_statement(parser);
  case TOKEN_FOR:
    return parser_parse_for_statement(parser);
  case TOKEN_DISPATCH:
    return parser_parse_dispatch_statement(parser);
  case TOKEN_SWITCH:
    return parser_parse_switch_statement(parser);
  case TOKEN_MATCH:
    return parser_parse_match_statement(parser);
  case TOKEN_BREAK:
    return parser_parse_break_statement(parser);
  case TOKEN_CONTINUE:
    return parser_parse_continue_statement(parser);
  case TOKEN_DEFER:
    return parser_parse_defer_statement(parser);
  case TOKEN_ERRDEFER:
    return parser_parse_errdefer_statement(parser);
  case TOKEN_ASM:
    return parser_parse_inline_asm(parser);
  case TOKEN_LBRACE:
    return parser_parse_block(parser);
  default:
    break;
  }

  if (parser->current_token.type == TOKEN_LPAREN &&
      parser->peek_token.type == TOKEN_IDENTIFIER) {
    return parser_parse_parenthesized_assignment(parser);
  }

  if (parser_reject_foreign_declaration(parser))
    return NULL;

  ASTNode *expr = parser_parse_expression(parser);
  if (!expr) {
    return NULL;
  }

  if (parser_is_assignment_token(parser->current_token.type)) {
    return parser_parse_assignment_from_target(parser, expr);
  }

  return expr;
}

static ASTNode *parser_parse_defer_or_errdefer(Parser *parser,
                                               TokenType token_type) {
  if (!parser) {
    return NULL;
  }

  SourceLocation location = parser_current_location(parser);
  const char *keyword =
      (token_type == TOKEN_DEFER) ? "defer" : "errdefer";

  if (!parser_expect(parser, token_type)) {
    return NULL;
  }

  if (parser->current_token.type == TOKEN_SEMICOLON ||
      parser->current_token.type == TOKEN_NEWLINE) {
    char msg[PARSER_ERROR_BUF_SIZE];
    snprintf(msg, sizeof(msg), "Expected statement after '%s'", keyword);
    parser_set_error(parser, msg);
    return NULL;
  }

  ASTNode *stmt = parser_parse_statement(parser);
  if (!stmt) {
    if (!parser->has_error) {
      char msg[PARSER_ERROR_BUF_SIZE];
      snprintf(msg, sizeof(msg), "Expected statement after '%s'", keyword);
      parser_set_error(parser, msg);
    }
    return NULL;
  }

  parser_expect_statement_end(parser);
  if (token_type == TOKEN_DEFER) {
    return ast_create_defer_statement(stmt, location);
  }
  return ast_create_errdefer_statement(stmt, location);
}

ASTNode *parser_parse_defer_statement(Parser *parser) {
  return parser_parse_defer_or_errdefer(parser, TOKEN_DEFER);
}

ASTNode *parser_parse_errdefer_statement(Parser *parser) {
  return parser_parse_defer_or_errdefer(parser, TOKEN_ERRDEFER);
}

void parser_report_expression_too_deep(Parser *parser) {
  char message[PARSER_ERROR_BUF_SIZE];
  snprintf(message, sizeof(message),
           "Expression nests more than %d levels deep",
           PARSER_MAX_EXPRESSION_DEPTH);
  parser_set_error_with_suggestion(
      parser, message,
      "split it into named parts: each `var` holding a sub-expression is one "
      "level less");
}

void parser_report_block_too_deep(Parser *parser) {
  char message[PARSER_ERROR_BUF_SIZE];
  snprintf(message, sizeof(message), "Blocks nest more than %d levels deep",
           PARSER_MAX_EXPRESSION_DEPTH);
  parser_set_error_with_suggestion(
      parser, message,
      "lift the inner blocks into functions of their own");
}

ASTNode *parser_parse_expression(Parser *parser) {
  if (!parser)
    return NULL;

  if (parser->expression_depth >= PARSER_MAX_EXPRESSION_DEPTH) {
    parser_report_expression_too_deep(parser);
    return NULL;
  }

  parser->expression_depth++;
  ASTNode *expression = parser_parse_binary_expression(parser, 0);
  parser->expression_depth--;
  return expression;
}

int parser_is_identifier_like(TokenType type) {
  // Check if token can be used as an identifier in expression context
  return type == TOKEN_IDENTIFIER ||
         // x86 mnemonics can be used as function names
         (type >= TOKEN_MOV && type <= TOKEN_SYSCALL) ||
         // x86 registers can be used as identifiers in high-level context
         (type >= TOKEN_EAX && type <= TOKEN_R15);
}

/* A nested type argument list closes with `>>`, which the lexer reads as one
 * right-shift token: `Pair<Box<int32>, Box<int32>>` failed to parse while the
 * same type spelled `... Box<int32> >` succeeded.
 *
 * These two split it. The inner list takes one `>` from the pair by rewriting
 * the token in place to a single `>` WITHOUT advancing, leaving that `>` as the
 * current token for the enclosing list to consume normally. Deeper nests fall
 * out of the same rule: `>>>>` is RSHIFT RSHIFT, and each level takes one `>`
 * in turn. Only the type is rewritten -- the token's text stays ">>", which
 * nothing in this path reads -- so a speculative parse that backtracks restores
 * the original token from its clone and loses the rewrite with it. */
static int parser_at_type_arg_close(const Parser *parser) {
  return parser->current_token.type == TOKEN_GREATER_THAN ||
         parser->current_token.type == TOKEN_RSHIFT;
}

static int parser_consume_type_arg_close(Parser *parser) {
  if (parser->current_token.type == TOKEN_GREATER_THAN) {
    parser_advance(parser);
    return 1;
  }
  if (parser->current_token.type == TOKEN_RSHIFT) {
    parser->current_token.type = TOKEN_GREATER_THAN;
    return 1;
  }
  return 0;
}

int parser_is_type_keyword(TokenType type) {
  // Check if token is a built-in type keyword
  return (type >= TOKEN_INT8 && type <= TOKEN_STRING_TYPE);
}

/* Built-in type names the lexer carries as plain identifiers rather than
   keywords. Without them `(cstring)&p` reads as `cstring & p` -- a bitwise and
   of the reflection value named `cstring` -- because the cast/grouping
   disambiguation below only trusts a leading type keyword. Naming the three
   here keeps the token set unchanged. */
int parser_is_builtin_type_name(const char *text) {
  return text && (strcmp(text, "bool") == 0 || strcmp(text, "cstring") == 0 ||
                  strcmp(text, "rawptr") == 0 || strcmp(text, "float16") == 0 ||
                  strcmp(text, "bfloat16") == 0);
}

static void parser_free_string_array(char **values, size_t count) {
  if (!values) {
    return;
  }

  for (size_t i = 0; i < count; i++) {
    free(values[i]);
  }
  free(values);
}

static void parser_free_type_param_list(char **params, char **traits,
                                        size_t count) {
  parser_free_string_array(params, count);
  parser_free_string_array(traits, count);
}

static char *parser_parse_qualified_name(Parser *parser, const char *expected);

static int parser_append_type_param_bound(char **bounds,
                                          const char *trait_name) {
  char *combined = NULL;
  size_t len = 0;

  if (!bounds || !trait_name) {
    return 0;
  }

  if (!*bounds) {
    *bounds = strdup(trait_name);
    return *bounds != NULL;
  }

  len = strlen(*bounds) + 1 + strlen(trait_name) + 1;
  combined = malloc(len);
  if (!combined) {
    return 0;
  }

  snprintf(combined, len, "%s+%s", *bounds, trait_name);
  free(*bounds);
  *bounds = combined;
  return 1;
}

static int parser_find_type_param_index(char **params, size_t count,
                                        const char *name, size_t *out_index) {
  if (!params || !name || !out_index) {
    return 0;
  }

  for (size_t i = 0; i < count; i++) {
    if (params[i] && strcmp(params[i], name) == 0) {
      *out_index = i;
      return 1;
    }
  }

  return 0;
}

static int parser_parse_bound_list(Parser *parser, char **out_bounds) {
  if (!parser || !out_bounds) {
    return 0;
  }

  do {
    char *trait_name =
        parser_parse_qualified_name(parser, "Expected trait name in bound");
    if (!trait_name) {
      return 0;
    }

    if (!parser_append_type_param_bound(out_bounds, trait_name)) {
      free(trait_name);
      parser_set_error(parser, "Out of memory while parsing trait bounds");
      return 0;
    }
    free(trait_name);

    if (parser->current_token.type != TOKEN_PLUS) {
      break;
    }
    parser_advance(parser);
  } while (1);

  return 1;
}

static int parser_parse_where_clause(Parser *parser, char **type_params,
                                     char **type_param_traits,
                                     size_t type_param_count) {
  if (!parser || parser->current_token.type != TOKEN_WHERE) {
    return 1;
  }

  parser_advance(parser); // consume 'where'

  do {
    size_t param_index = 0;
    char *param_name = NULL;

    if (!parser_is_identifier_like(parser->current_token.type)) {
      parser_set_error(parser, "Expected type parameter name in where clause");
      return 0;
    }

    param_name = strdup(parser->current_token.value);
    if (!param_name) {
      parser_set_error(parser, "Out of memory while parsing where clause");
      return 0;
    }
    parser_advance(parser);

    if (!parser_find_type_param_index(type_params, type_param_count, param_name,
                                      &param_index)) {
      parser_set_error(parser,
                       "Where clause references unknown type parameter");
      free(param_name);
      return 0;
    }
    free(param_name);

    if (!parser_expect(parser, TOKEN_COLON)) {
      return 0;
    }

    if (!parser_parse_bound_list(parser, &type_param_traits[param_index])) {
      return 0;
    }

    if (parser->current_token.type != TOKEN_COMMA) {
      break;
    }
    parser_advance(parser);
  } while (1);

  return 1;
}

static char *parser_parse_qualified_name(Parser *parser, const char *expected) {
  char *name = NULL;

  if (!parser || !expected) {
    return NULL;
  }

  if (!parser_is_identifier_like(parser->current_token.type) &&
      !parser_is_type_keyword(parser->current_token.type)) {
    parser_set_error(parser, expected);
    return NULL;
  }

  name = strdup(parser->current_token.value);
  if (!name) {
    return NULL;
  }
  parser_advance(parser);

  while (parser->current_token.type == TOKEN_DOT) {
    char *qualified_name = NULL;
    size_t qualified_len = 0;

    parser_advance(parser);
    if (!parser_is_identifier_like(parser->current_token.type) &&
        !parser_is_type_keyword(parser->current_token.type)) {
      parser_set_error(parser, expected);
      free(name);
      return NULL;
    }

    qualified_len = strlen(name) + 1 + strlen(parser->current_token.value) + 1;
    qualified_name = malloc(qualified_len);
    if (!qualified_name) {
      free(name);
      return NULL;
    }

    snprintf(qualified_name, qualified_len, "%s.%s", name,
             parser->current_token.value);
    free(name);
    name = qualified_name;
    parser_advance(parser);
  }

  return name;
}

static char **parser_parse_type_param_list(Parser *parser, char ***out_traits,
                                           size_t *out_count) {
  *out_count = 0;
  *out_traits = NULL;
  if (parser->current_token.type != TOKEN_LESS_THAN)
    return NULL;
  parser_advance(parser); // consume '<'

  char **params = NULL;
  char **traits = NULL;
  size_t count = 0;

  while (parser->current_token.type != TOKEN_GREATER_THAN &&
         parser->current_token.type != TOKEN_EOF) {
    if (!parser_is_identifier_like(parser->current_token.type)) {
      parser_set_error(parser, "Expected type parameter name");
      parser_free_type_param_list(params, traits, count);
      return NULL;
    }
    params = realloc(params, (count + 1) * sizeof(char *));
    traits = realloc(traits, (count + 1) * sizeof(char *));
    params[count] = strdup(parser->current_token.value);
    traits[count] = NULL;
    count++;
    parser_advance(parser);

    if (parser->current_token.type == TOKEN_COLON) {
      parser_advance(parser);
      if (!parser_parse_bound_list(parser, &traits[count - 1])) {
        parser_free_type_param_list(params, traits, count);
        return NULL;
      }
    }

    if (parser->current_token.type == TOKEN_COMMA) {
      parser_advance(parser);
    } else if (parser->current_token.type != TOKEN_GREATER_THAN) {
      parser_set_error(parser, "Expected ',' or '>' in type parameter list");
      parser_free_type_param_list(params, traits, count);
      return NULL;
    }
  }

  if (!parser_expect(parser, TOKEN_GREATER_THAN)) {
    parser_free_type_param_list(params, traits, count);
    return NULL;
  }

  *out_count = count;
  *out_traits = traits;
  return params;
}

static char *parser_parse_type_annotation(Parser *parser);
static char *parser_parse_type_annotation_ex(Parser *parser, int allow_array);

/* `volatile T`: every access to a T is observable in itself, so none may be
 * removed, merged, reordered against another volatile access, or served from
 * a register. Spelled as a prefix qualifier and carried in the type text. */
static int parser_at_volatile_type(Parser *parser) {
  return parser_is_identifier_like(parser->current_token.type) &&
         parser->current_token.value &&
         strcmp(parser->current_token.value, "volatile") == 0 &&
         parser->peek_token.type != TOKEN_COLON &&
         parser->peek_token.type != TOKEN_COMMA &&
         parser->peek_token.type != TOKEN_RPAREN;
}

static char *parser_parse_volatile_type(Parser *parser) {
  char *inner;
  char *qualified;
  parser_advance(parser);
  inner = parser_parse_type_annotation(parser);
  if (!inner) {
    return NULL;
  }
  if (strncmp(inner, "volatile ", 9) == 0) {
    return inner;
  }
  qualified = malloc(strlen(inner) + 10);
  if (!qualified) {
    free(inner);
    return NULL;
  }
  sprintf(qualified, "volatile %s", inner);
  free(inner);
  return qualified;
}

/* Function pointer type: fn(param_types) -> return_type (thin), or
 * Fn(param_types) -> return_type (a stateful closure type). */
static int parser_at_closure_type(Parser *parser) {
  return parser_is_identifier_like(parser->current_token.type) &&
         parser->current_token.value &&
         strcmp(parser->current_token.value, "Fn") == 0 &&
         parser->peek_token.type == TOKEN_LPAREN;
}

static char *parser_parse_type_effect_clauses(Parser *parser, char *type_name) {
  static const char *const words[2] = {"with", "requires"};
  int seen[2] = {0, 0};
  for (;;) {
    int clause = -1;
    char **names = NULL;
    size_t count = 0;
    size_t length;
    if (parser_is_identifier_like(parser->current_token.type) &&
        parser->current_token.value) {
      for (int c = 0; c < 2; c++) {
        if (strcmp(parser->current_token.value, words[c]) == 0) {
          clause = c;
        }
      }
      if (clause < 0 && (strcmp(parser->current_token.value, "forbids") == 0 ||
                         strcmp(parser->current_token.value, "provides") == 0)) {
        parser_set_error(parser,
                         "a function type carries 'with' and 'requires'; "
                         "'forbids' and 'provides' describe a body, and a "
                         "type has none");
        free(type_name);
        return NULL;
      }
    }
    if (clause < 0) {
      return type_name;
    }
    if (seen[clause]) {
      parser_set_error(parser, "this clause appears twice on the function "
                               "type; list every effect in the one clause");
      free(type_name);
      return NULL;
    }
    seen[clause] = 1;
    parser_advance(parser);
    if (!parser_parse_effect_name_list(parser, &names, &count)) {
      free(type_name);
      return NULL;
    }
    length = strlen(type_name) + 1 + strlen(words[clause]) + 1;
    for (size_t i = 0; i < count; i++) {
      length += strlen(names[i]) + 1;
    }
    {
      char *grown = realloc(type_name, length + 1);
      if (!grown) {
        parser_free_string_array(names, count);
        free(type_name);
        return NULL;
      }
      type_name = grown;
    }
    strcat(type_name, " ");
    strcat(type_name, words[clause]);
    strcat(type_name, " ");
    for (size_t i = 0; i < count; i++) {
      if (i) {
        strcat(type_name, ",");
      }
      strcat(type_name, names[i]);
    }
    parser_free_string_array(names, count);
  }
}

static char *parser_parse_function_pointer_type(Parser *parser,
                                                int is_closure_fn) {
  char *params_buf;
  char *ret;
  char *type_name;
  size_t params_len = 0;
  size_t params_cap = 1024;
  size_t fn_len;
  int first = 1;

  parser_advance(parser); /* consume 'fn' or 'Fn' */
  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_set_error(parser,
                     "Expected '(' after 'fn' in function pointer type");
    return NULL;
  }
  params_buf = malloc(params_cap);
  if (!params_buf)
    return NULL;
  params_buf[0] = '\0';
  while (parser->current_token.type != TOKEN_RPAREN &&
         parser->current_token.type != TOKEN_EOF && !parser->has_error) {
    char *param;
    size_t plen;
    if (!first) {
      if (parser->current_token.type != TOKEN_COMMA) {
        parser_set_error(
            parser, "Expected ',' or ')' in function pointer parameter list");
        free(params_buf);
        return NULL;
      }
      parser_advance(parser); /* consume ',' */
      if (params_len + 1 >= params_cap) {
        params_cap *= 2;
        params_buf = realloc(params_buf, params_cap);
        if (!params_buf)
          return NULL;
      }
      params_buf[params_len++] = ',';
      params_buf[params_len] = '\0';
    }
    first = 0;
    param = parser_parse_type_annotation(parser);
    if (!param) {
      if (!parser->has_error)
        parser_set_error(parser,
                         "Expected type in function pointer parameter list");
      free(params_buf);
      return NULL;
    }
    plen = strlen(param);
    while (params_len + plen + 1 >= params_cap) {
      params_cap *= 2;
      params_buf = realloc(params_buf, params_cap);
      if (!params_buf) {
        free(param);
        return NULL;
      }
    }
    memcpy(params_buf + params_len, param, plen + 1);
    params_len += plen;
    free(param);
  }
  if (!parser_expect(parser, TOKEN_RPAREN)) {
    free(params_buf);
    return NULL;
  }
  if (!parser_expect(parser, TOKEN_ARROW)) {
    parser_set_error(parser, "Expected '->' after ')' in function pointer type");
    free(params_buf);
    return NULL;
  }
  ret = parser_parse_type_annotation(parser);
  if (!ret) {
    if (!parser->has_error)
      parser_set_error(
          parser, "Expected return type after '->' in function pointer type");
    free(params_buf);
    return NULL;
  }
  /* "fn(" + params + ")->" + ret + NUL */
  fn_len = 4 + params_len + 3 + strlen(ret) + 1;
  type_name = malloc(fn_len);
  if (!type_name) {
    free(params_buf);
    free(ret);
    return NULL;
  }
  snprintf(type_name, fn_len, is_closure_fn ? "Fn(%s)->%s" : "fn(%s)->%s",
           params_buf, ret);
  free(params_buf);
  free(ret);
  return parser_parse_type_effect_clauses(parser, type_name);
}

static char *parser_parse_qualified_type(Parser *parser, char *type_name) {
  while (parser->current_token.type == TOKEN_DOT) {
    char *qualified_type;
    size_t qualified_len;

    parser_advance(parser); /* consume '.' */
    if (!parser_is_identifier_like(parser->current_token.type) &&
        !parser_is_type_keyword(parser->current_token.type)) {
      parser_set_error(parser, "Expected type name after '.'");
      free(type_name);
      return NULL;
    }

    qualified_len =
        strlen(type_name) + 1 + strlen(parser->current_token.value) + 1;
    qualified_type = malloc(qualified_len);
    if (!qualified_type) {
      free(type_name);
      return NULL;
    }

    snprintf(qualified_type, qualified_len, "%s.%s", type_name,
             parser->current_token.value);
    free(type_name);
    type_name = qualified_type;
    parser_advance(parser);
  }
  return type_name;
}

static char *parser_parse_type_arguments(Parser *parser, char *type_name) {
  char *args_buf;
  char *full_type;
  size_t args_len = 0;
  size_t args_cap = 1024;
  size_t full_len;
  int first = 1;

  if (parser->current_token.type != TOKEN_LESS_THAN) {
    return type_name;
  }
  parser_advance(parser); /* consume '<' */

  args_buf = malloc(args_cap);
  args_buf[0] = '\0';

  while (!parser_at_type_arg_close(parser) &&
         parser->current_token.type != TOKEN_EOF && !parser->has_error) {
    char *arg;
    size_t arg_len;
    if (!first) {
      if (parser->current_token.type != TOKEN_COMMA) {
        parser_set_error(parser, "Expected ',' or '>' in type argument list");
        free(args_buf);
        free(type_name);
        return NULL;
      }
      parser_advance(parser); /* consume ',' */

      if (args_len + 1 >= args_cap) {
        char *new_args_buf;
        args_cap *= 2;
        new_args_buf = realloc(args_buf, args_cap);
        if (!new_args_buf) {
          free(args_buf);
          free(type_name);
          return NULL;
        }
        args_buf = new_args_buf;
      }
      args_buf[args_len++] = ',';
      args_buf[args_len] = '\0';
    }
    first = 0;

    arg = parser_parse_type_annotation(parser);
    if (!arg) {
      if (!parser->has_error)
        parser_set_error(parser, "Expected type in type argument list");
      free(args_buf);
      free(type_name);
      return NULL;
    }

    arg_len = strlen(arg);
    while (args_len + arg_len + 1 >= args_cap) {
      char *new_args_buf;
      args_cap *= 2;
      new_args_buf = realloc(args_buf, args_cap);
      if (!new_args_buf) {
        free(arg);
        free(args_buf);
        free(type_name);
        return NULL;
      }
      args_buf = new_args_buf;
    }
    memcpy(args_buf + args_len, arg, arg_len);
    args_len += arg_len;
    args_buf[args_len] = '\0';
    free(arg);
  }

  if (!parser_consume_type_arg_close(parser)) {
    parser_set_error(parser, "Expected '>' to close the type argument list");
    free(args_buf);
    free(type_name);
    return NULL;
  }

  full_len = strlen(type_name) + 1 + args_len + 1 + 1;
  full_type = malloc(full_len);
  snprintf(full_type, full_len, "%s<%s>", type_name, args_buf);
  free(type_name);
  free(args_buf);
  return full_type;
}

static char *parser_parse_pointer_suffix(Parser *parser, char *type_name) {
  while (parser->current_token.type == TOKEN_MULTIPLY) {
    size_t next_len = strlen(type_name) + 2;
    char *next_type = malloc(next_len);
    if (!next_type) {
      free(type_name);
      return NULL;
    }

    snprintf(next_type, next_len, "%s*", type_name);
    free(type_name);
    type_name = next_type;
    parser_advance(parser); /* consume '*' */
  }
  return type_name;
}

static char *parser_parse_array_suffix(Parser *parser, char *type_name) {
  char *size_text;
  char *full_type;
  size_t full_len;

  if (parser->current_token.type != TOKEN_LBRACKET) {
    return type_name;
  }
  parser_advance(parser); /* consume '[' */

  /* `T[..]`: a gathered parameter. The brackets say array and the `..` says the
     length comes from the call, which is what a variadic parameter is. Inside
     the function it is an ordinary `T[]`. */
  if (parser->current_token.type == TOKEN_DOT_DOT) {
    size_t rest_len = strlen(type_name) + 5;
    char *rest_type = malloc(rest_len);
    parser_advance(parser);
    if (!rest_type) {
      free(type_name);
      return NULL;
    }
    snprintf(rest_type, rest_len, "%s[..]", type_name);
    free(type_name);
    if (!parser_expect(parser, TOKEN_RBRACKET)) {
      free(rest_type);
      return NULL;
    }
    return rest_type;
  }

  /* `T[]`: a slice, whose length is not part of the type. Empty brackets are
     what says so, and the suffix keeps stacking after it. */
  if (parser->current_token.type == TOKEN_RBRACKET) {
    size_t slice_len = strlen(type_name) + 3;
    char *slice_type = malloc(slice_len);
    parser_advance(parser);
    if (!slice_type) {
      free(type_name);
      return NULL;
    }
    snprintf(slice_type, slice_len, "%s[]", type_name);
    free(type_name);
    return parser_parse_array_suffix(parser, slice_type);
  }

  if (parser->current_token.type == TOKEN_COMMA) {
    size_t commas = 0;
    size_t view_len;
    char *view_type;
    while (parser->current_token.type == TOKEN_COMMA) {
      commas++;
      parser_advance(parser);
    }
    if (!parser_expect(parser, TOKEN_RBRACKET)) {
      free(type_name);
      return NULL;
    }
    view_len = strlen(type_name) + commas + 3;
    view_type = malloc(view_len);
    if (!view_type) {
      free(type_name);
      return NULL;
    }
    snprintf(view_type, view_len, "%s[", type_name);
    for (size_t i = 0; i < commas; i++) {
      strcat(view_type, ",");
    }
    strcat(view_type, "]");
    free(type_name);
    return parser_parse_array_suffix(parser, view_type);
  }

  if (parser->current_token.type != TOKEN_NUMBER &&
      parser->current_token.type != TOKEN_IDENTIFIER) {
    free(type_name);
    parser_set_error(parser, "Expected array size after '['");
    return NULL;
  }

  size_text = strdup(parser->current_token.value);
  parser_advance(parser);

  if (!size_text) {
    free(type_name);
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_RBRACKET)) {
    free(type_name);
    free(size_text);
    return NULL;
  }

  full_len = strlen(type_name) + strlen(size_text) + 3;
  full_type = malloc(full_len);
  if (!full_type) {
    free(type_name);
    free(size_text);
    return NULL;
  }

  snprintf(full_type, full_len, "%s[%s]", type_name, size_text);
  free(type_name);
  free(size_text);
  /* Another '[' after a sized one is a further dimension: `int32[3][4]` is
     three rows of four, the way the declaration reads. The slice form above
     already stacked; only this branch stopped, which is what made a second
     dimension a syntax error rather than a type. */
  return parser_parse_array_suffix(parser, full_type);
}

static char *parser_parse_type_annotation_ex(Parser *parser, int allow_array) {
  char *type_name = NULL;
  int is_closure_fn;

  if (!parser)
    return NULL;

  if (parser_at_volatile_type(parser)) {
    return parser_parse_volatile_type(parser);
  }

  /* A parenthesised type groups what a suffix binds to. `fn(int32) -> int32[2]`
   * reads the array as the return type, because that is where the suffix sits;
   * `(fn(int32) -> int32)[2]` is the array of function pointers. The
   * parentheses stay in the text so the resolver sees the same grouping. */
  if (parser->current_token.type == TOKEN_LPAREN) {
    char *inner;
    char *grouped;
    size_t grouped_len;
    parser_advance(parser);
    inner = parser_parse_type_annotation(parser);
    if (!inner) {
      parser_set_error(parser, "Expected a type after '(' in a type");
      return NULL;
    }
    if (parser->current_token.type != TOKEN_RPAREN) {
      parser_set_error(parser, "Expected ')' after a parenthesised type");
      free(inner);
      return NULL;
    }
    parser_advance(parser);
    grouped_len = strlen(inner) + 3;
    grouped = malloc(grouped_len);
    if (!grouped) {
      free(inner);
      return NULL;
    }
    snprintf(grouped, grouped_len, "(%s)", inner);
    free(inner);
    grouped = parser_parse_pointer_suffix(parser, grouped);
    if (!grouped) {
      return NULL;
    }
    return allow_array ? parser_parse_array_suffix(parser, grouped) : grouped;
  }

  is_closure_fn = parser_at_closure_type(parser);
  if (parser->current_token.type == TOKEN_FN || is_closure_fn) {
    type_name = parser_parse_function_pointer_type(parser, is_closure_fn);
  } else if (parser_at_composed_name(parser)) {
    /* A type annotation is a name the checker resolves, and resolution happens
     * before the binding this would be composed from has a value. Naming the
     * boundary beats a parse error further along that points at the wrong
     * token. */
    parser_set_error(parser,
                     "'ident(...)' composes a declaration's name, not a type; "
                     "write a generated type's name out where you use it");
    return NULL;
  } else if (!parser_is_type_keyword(parser->current_token.type) &&
             !parser_is_identifier_like(parser->current_token.type)) {
    return NULL;
  } else {
    type_name = strdup(parser->current_token.value);
    parser_advance(parser);
  }
  if (!type_name) {
    return NULL;
  }

  type_name = parser_parse_qualified_type(parser, type_name);
  if (!type_name) {
    return NULL;
  }
  type_name = parser_parse_type_arguments(parser, type_name);
  if (!type_name) {
    return NULL;
  }
  type_name = parser_parse_pointer_suffix(parser, type_name);
  if (!type_name) {
    return NULL;
  }
  return allow_array ? parser_parse_array_suffix(parser, type_name) : type_name;
}

static char *parser_parse_type_annotation(Parser *parser) {
  return parser_parse_type_annotation_ex(parser, 1);
}

static int parser_literal_radix_hint(const char *value) {
  if (!value || value[0] != '0' || value[1] == '\0') {
    return 10;
  }
  if (value[1] == 'x' || value[1] == 'X') {
    return 16;
  }
  if (value[1] == 'b' || value[1] == 'B') {
    return 2;
  }
  /* Leading zeros without 0x/0b prefix are still decimal (e.g. "007"). */
  return 10;
}

typedef enum {
  PARSER_LITERAL_OK = 0,
  PARSER_LITERAL_INVALID,
  PARSER_LITERAL_OVERFLOW
} ParserLiteralScan;

static ParserLiteralScan parser_scan_unsigned_literal(
    const char *text, unsigned base, unsigned long long *out_value) {
  const unsigned long long cutoff = ULLONG_MAX / base;
  const unsigned long long cutlim = ULLONG_MAX % base;
  unsigned long long value = 0;
  int any = 0;

  for (; *text != '\0'; text++) {
    char c = *text;
    unsigned digit;
    if (c >= '0' && c <= '9') {
      digit = (unsigned)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = (unsigned)(c - 'a') + 10u;
    } else if (c >= 'A' && c <= 'F') {
      digit = (unsigned)(c - 'A') + 10u;
    } else {
      return PARSER_LITERAL_INVALID;
    }
    if (digit >= base) {
      return PARSER_LITERAL_INVALID;
    }
    if (value > cutoff ||
        (value == cutoff && (unsigned long long)digit > cutlim)) {
      return PARSER_LITERAL_OVERFLOW;
    }
    value = value * base + (unsigned long long)digit;
    any = 1;
  }

  if (!any) {
    return PARSER_LITERAL_INVALID;
  }
  *out_value = value;
  return PARSER_LITERAL_OK;
}

/*
 * Parses integer TOKEN_NUMBER lexeme text into long long bits and radix.
 * Handles decimal, hexadecimal, and binary (0b) digits only. Overflow is
 * detected while accumulating rather than read back from errno: the owned
 * runtime's strtoull wraps modulo 2^64 and never reports ERANGE, so every
 * literal past UINT64_MAX used to be accepted with a wrapped value.
 */
static int parser_parse_integer_literal_string(
    Parser *parser, const char *value, long long *out_value,
    unsigned char *out_radix) {
  if (!parser || !value || !out_value || !out_radix) {
    return 0;
  }

  int hint = parser_literal_radix_hint(value);
  unsigned long long u = 0;
  ParserLiteralScan scan;

  if (hint == 16) {
    scan = parser_scan_unsigned_literal(value + 2, 16u, &u);
    if (scan == PARSER_LITERAL_INVALID) {
      parser_set_error(parser, "Invalid hexadecimal literal");
      return 0;
    }
    if (scan == PARSER_LITERAL_OVERFLOW) {
      parser_set_error(parser, "Hexadecimal literal is out of range");
      return 0;
    }
    *out_radix = 16;
    *out_value = (long long)u;
    return 1;
  }

  if (hint == 2) {
    scan = parser_scan_unsigned_literal(value + 2, 2u, &u);
    if (scan == PARSER_LITERAL_INVALID) {
      parser_set_error(parser, "Invalid binary literal");
      return 0;
    }
    if (scan == PARSER_LITERAL_OVERFLOW) {
      parser_set_error(parser, "Binary literal is out of range");
      return 0;
    }
    *out_radix = 2;
    *out_value = (long long)u;
    return 1;
  }

  /* Past LLONG_MAX but still a uint64 keeps the bit pattern, exactly as the
   * hexadecimal branch above does: without this a uint64 constant had to be
   * written in hex -- 18446744073709551615 was rejected while
   * 0xFFFFFFFFFFFFFFFF, the same value, was accepted. A leading '-' is not
   * part of the literal (it lexes as unary minus), so this only ever widens
   * the positive range. */
  scan = parser_scan_unsigned_literal(value, 10u, &u);
  if (scan == PARSER_LITERAL_INVALID) {
    parser_set_error(parser, "Invalid integer literal");
    return 0;
  }
  if (scan == PARSER_LITERAL_OVERFLOW) {
    parser_set_error(parser, "Decimal integer literal is out of range");
    return 0;
  }
  *out_radix = 10;
  *out_value = (long long)u;
  return 1;
}

static ASTNode *parser_parse_for_initializer(Parser *parser) {
  if (!parser)
    return NULL;

  if (parser->current_token.type == TOKEN_SEMICOLON) {
    return NULL;
  }

  if (parser->current_token.type == TOKEN_VAR) {
    return parser_parse_var_declaration(parser);
  }

  ASTNode *expr = parser_parse_expression(parser);
  if (!expr)
    return NULL;

  if (parser_is_assignment_token(parser->current_token.type)) {
    return parser_parse_assignment_from_target(parser, expr);
  }

  return expr;
}

static int parser_identifier_name_is(ASTNode *expr, const char *name) {
  if (!expr || expr->type != AST_IDENTIFIER || !name) {
    return 0;
  }

  Identifier *id = (Identifier *)expr->data;
  return id && id->name && strcmp(id->name, name) == 0;
}

static int parser_parse_parameter_list(Parser *parser, char ***out_names,
                                       char ***out_types, size_t *out_count);

static char **parser_parse_multi_return_types(Parser *parser,
                                              size_t *out_count) {
  if (!parser || !out_count || parser->current_token.type != TOKEN_LPAREN) {
    return NULL;
  }

  *out_count = 0;
  parser_advance(parser);
  size_t capacity = 4;
  char **types = calloc(capacity, sizeof(char *));
  if (!types) {
    return NULL;
  }

  while (parser->current_token.type != TOKEN_RPAREN &&
         parser->current_token.type != TOKEN_EOF && !parser->has_error) {
    char *type = parser_parse_type_annotation(parser);
    if (!type) {
      if (!parser->has_error) {
        parser_set_error(parser, "Expected return type in multiple return list");
      }
      parser_free_string_array(types, *out_count);
      return NULL;
    }
    if (*out_count == capacity) {
      capacity *= 2;
      char **grown = realloc(types, capacity * sizeof(char *));
      if (!grown) {
        free(type);
        parser_free_string_array(types, *out_count);
        return NULL;
      }
      types = grown;
    }
    types[(*out_count)++] = type;
    if (parser->current_token.type != TOKEN_COMMA) {
      break;
    }
    parser_advance(parser);
  }

  if (!parser_expect(parser, TOKEN_RPAREN)) {
    parser_free_string_array(types, *out_count);
    *out_count = 0;
    return NULL;
  }
  if (*out_count < 2) {
    parser_set_error(parser, "Multiple return types require at least two types");
    parser_free_string_array(types, *out_count);
    *out_count = 0;
    return NULL;
  }
  return types;
}

static char *parser_make_multi_return_name(const char *function_name) {
  const char *prefix = "__mettle_multi_return_";
  if (!function_name) {
    return NULL;
  }
  size_t length = strlen(prefix) + strlen(function_name) + 1;
  char *name = malloc(length);
  if (name) {
    snprintf(name, length, "%s%s", prefix, function_name);
  }
  return name;
}

/* Anonymous function (lambda) expression: `fn(params) [-> ret] { body }`. In
 * expression position `fn` always begins a lambda; the type spelling
 * `fn(...)->R` is parsed only in type positions by parser_parse_type_annotation,
 * and named declarations only at the top level via parse_declaration. The node
 * is an AST_LAMBDA_EXPRESSION carrying a FunctionDeclaration payload with a NULL
 * name; the closure-conversion pass lifts it to a real top-level function. */
static ASTNode *parser_parse_lambda_expression(Parser *parser) {
  SourceLocation location = parser_current_location(parser);
  parser_advance(parser); // consume 'fn'

  if (!parser_expect(parser, TOKEN_LPAREN)) {
    return NULL;
  }

  char **param_names = NULL;
  char **param_types = NULL;
  size_t param_count = 0;
  if (!parser_parse_parameter_list(parser, &param_names, &param_types,
                                   &param_count)) {
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_RPAREN)) {
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    return NULL;
  }

  char *return_type = NULL;
  if (parser->current_token.type == TOKEN_ARROW ||
      parser->current_token.type == TOKEN_COLON) {
    parser_advance(parser);
    return_type = parser_parse_type_annotation(parser);
    if (!return_type) {
      if (!parser->has_error) {
        parser_set_error(parser, "Expected return type after '->' in lambda");
      }
      for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
        free(param_types[i]);
      }
      free(param_names);
      free(param_types);
      return NULL;
    }
  } else {
    return_type = strdup("void");
  }

  if (parser->current_token.type != TOKEN_LBRACE) {
    parser_set_error(parser, "Expected '{' to begin lambda body");
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    free(return_type);
    return NULL;
  }

  ASTNode *body = parser_parse_block(parser);
  if (!body) {
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    free(return_type);
    return NULL;
  }

  ASTNode *node = ast_create_function_declaration(
      NULL, param_names, param_types, param_count, return_type, body, location);
  for (size_t i = 0; i < param_count; i++) {
    free(param_names[i]);
    free(param_types[i]);
  }
  free(param_names);
  free(param_types);
  free(return_type);
  if (!node) {
    ast_destroy_node(body);
    return NULL;
  }
  node->type = AST_LAMBDA_EXPRESSION;
  return node;
}

/* Free the half-built element/name arrays of an aggregate literal that failed
 * to parse. The element nodes are not children of anything yet, so they have to
 * go too. */
static void parser_free_aggregate_parts(ASTNode **elements, char **field_names,
                                        size_t count) {
  for (size_t i = 0; i < count; i++) {
    ast_destroy_node(elements ? elements[i] : NULL);
    free(field_names ? field_names[i] : NULL);
  }
  free(elements);
  free(field_names);
}

static int parser_aggregate_push(ASTNode ***elements, char ***field_names,
                                 size_t *count, size_t *capacity,
                                 ASTNode *element, char *field_name) {
  if (*count == *capacity) {
    size_t next = *capacity ? *capacity * 2 : 8;
    ASTNode **grown_elements =
        realloc(*elements, next * sizeof(**elements));
    if (!grown_elements) {
      return 0;
    }
    *elements = grown_elements;
    if (field_names) {
      char **grown_names = realloc(*field_names, next * sizeof(**field_names));
      if (!grown_names) {
        return 0;
      }
      *field_names = grown_names;
    }
    *capacity = next;
  }
  (*elements)[*count] = element;
  if (field_names) {
    (*field_names)[*count] = field_name;
  }
  (*count)++;
  return 1;
}

/* `[ a, b, c ]` or `[ value; count ]`. The lexer suppresses newlines inside
 * brackets, so a table may be written across as many lines as it needs. */
static ASTNode *parser_parse_array_literal(Parser *parser,
                                           SourceLocation location) {
  ASTNode **elements = NULL;
  size_t count = 0;
  size_t capacity = 0;
  ASTNode *repeat_count = NULL;

  parser_advance(parser); // consume '['

  if (parser->current_token.type != TOKEN_RBRACKET) {
    for (;;) {
      ASTNode *element = parser_parse_expression(parser);
      if (!element) {
        parser_free_aggregate_parts(elements, NULL, count);
        return NULL;
      }
      if (!parser_aggregate_push(&elements, NULL, &count, &capacity, element,
                                 NULL)) {
        ast_destroy_node(element);
        parser_free_aggregate_parts(elements, NULL, count);
        parser_set_error(parser, "Out of memory in array literal");
        return NULL;
      }

      /* `[value; count]` repeats one element; it is only meaningful as the
       * whole literal, so it may appear exactly once, after the first. */
      if (parser->current_token.type == TOKEN_SEMICOLON) {
        if (count != 1) {
          parser_set_error(parser,
                           "Repeat count in an array literal must follow a "
                           "single element: write '[value; count]'");
          parser_free_aggregate_parts(elements, NULL, count);
          return NULL;
        }
        parser_advance(parser); // consume ';'
        repeat_count = parser_parse_expression(parser);
        if (!repeat_count) {
          parser_free_aggregate_parts(elements, NULL, count);
          return NULL;
        }
        break;
      }

      if (parser->current_token.type != TOKEN_COMMA) {
        break;
      }
      parser_advance(parser); // consume ','
      if (parser->current_token.type == TOKEN_RBRACKET) {
        break; // trailing comma
      }
    }
  }

  if (!parser_expect(parser, TOKEN_RBRACKET)) {
    ast_destroy_node(repeat_count);
    parser_free_aggregate_parts(elements, NULL, count);
    return NULL;
  }

  ASTNode *node = ast_create_aggregate_literal(0, elements, NULL, count,
                                               repeat_count, location);
  if (!node) {
    ast_destroy_node(repeat_count);
    parser_free_aggregate_parts(elements, NULL, count);
    parser_set_error(parser, "Out of memory in array literal");
    return NULL;
  }
  return node;
}

/* `{ field: value, ... }`. Braces do not suppress newlines (they delimit
 * blocks, where newlines end statements), so this skips them explicitly and a
 * struct literal may span lines. */
static ASTNode *parser_parse_struct_literal(Parser *parser,
                                            SourceLocation location) {
  ASTNode **elements = NULL;
  char **field_names = NULL;
  size_t count = 0;
  size_t capacity = 0;

  parser_advance(parser); // consume '{'
  while (parser->current_token.type == TOKEN_NEWLINE) {
    parser_advance(parser);
  }

  if (parser->current_token.type != TOKEN_RBRACE) {
    for (;;) {
      if (!parser_is_identifier_like(parser->current_token.type)) {
        parser_set_error(parser,
                         "Expected a field name in the struct literal: write "
                         "'{ field: value, ... }'");
        parser_free_aggregate_parts(elements, field_names, count);
        return NULL;
      }
      char *field_name = strdup(parser->current_token.value);
      if (!field_name) {
        parser_set_error(parser, "Out of memory in struct literal");
        parser_free_aggregate_parts(elements, field_names, count);
        return NULL;
      }
      parser_advance(parser); // consume the field name
      if (!parser_expect(parser, TOKEN_COLON)) {
        free(field_name);
        parser_free_aggregate_parts(elements, field_names, count);
        return NULL;
      }

      ASTNode *element = parser_parse_expression(parser);
      if (!element) {
        free(field_name);
        parser_free_aggregate_parts(elements, field_names, count);
        return NULL;
      }
      if (!parser_aggregate_push(&elements, &field_names, &count, &capacity,
                                 element, field_name)) {
        ast_destroy_node(element);
        free(field_name);
        parser_free_aggregate_parts(elements, field_names, count);
        parser_set_error(parser, "Out of memory in struct literal");
        return NULL;
      }

      while (parser->current_token.type == TOKEN_NEWLINE) {
        parser_advance(parser);
      }
      if (parser->current_token.type != TOKEN_COMMA) {
        break;
      }
      parser_advance(parser); // consume ','
      while (parser->current_token.type == TOKEN_NEWLINE) {
        parser_advance(parser);
      }
      if (parser->current_token.type == TOKEN_RBRACE) {
        break; // trailing comma
      }
    }
  }

  if (!parser_expect(parser, TOKEN_RBRACE)) {
    parser_free_aggregate_parts(elements, field_names, count);
    return NULL;
  }

  ASTNode *node =
      ast_create_aggregate_literal(1, elements, field_names, count, NULL,
                                   location);
  if (!node) {
    parser_free_aggregate_parts(elements, field_names, count);
    parser_set_error(parser, "Out of memory in struct literal");
    return NULL;
  }
  return node;
}

// Name the token in front of the reader as concretely as the token allows:
// its own text when it has text worth printing, otherwise its class.
static void parser_describe_token(const Token *token, char *out, size_t cap) {
  switch (token->type) {
  case TOKEN_EOF:
    snprintf(out, cap, "the end of the file");
    return;
  case TOKEN_NEWLINE:
    snprintf(out, cap, "the end of the line");
    return;
  case TOKEN_STRING:
    snprintf(out, cap, "a string literal");
    return;
  case TOKEN_ERROR:
    snprintf(out, cap, "an invalid token");
    return;
  default:
    break;
  }
  if (token->value && token->value[0] != '\0') {
    snprintf(out, cap, "'%s'", token->value);
    return;
  }
  snprintf(out, cap, "%s", token_type_to_string(token->type));
}

// True for keywords that open a statement. One of these in expression
// position is nearly always a missing operand, not a misspelt name.
static int parser_token_starts_statement(TokenType type) {
  switch (type) {
  case TOKEN_IF:
  case TOKEN_ELSE:
  case TOKEN_WHILE:
  case TOKEN_FOR:
  case TOKEN_RETURN:
  case TOKEN_BREAK:
  case TOKEN_CONTINUE:
  case TOKEN_SWITCH:
  case TOKEN_CASE:
  case TOKEN_DEFAULT:
  case TOKEN_VAR:
  case TOKEN_CONST:
  case TOKEN_FN:
  case TOKEN_FUNCTION:
  case TOKEN_STRUCT:
  case TOKEN_ENUM:
  case TOKEN_IMPORT:
  case TOKEN_EXPORT:
  case TOKEN_DEFER:
  case TOKEN_ERRDEFER:
    return 1;
  default:
    return 0;
  }
}

// The expression parser ran out of grammar. Say what it was reading, what it
// found instead, and what to write. This is the last stop for a large share
// of the syntax errors a reader ever sees, so it earns the detail.
static void parser_error_expected_expression(Parser *parser) {
  char found[PARSER_PREV_TEXT_MAX + 24];
  parser_describe_token(&parser->current_token, found, sizeof(found));

  // What the parser had just read. It names the operator or keyword whose
  // operand went missing, which is where the reader has to type.
  char after[PARSER_PREV_TEXT_MAX + 16];
  after[0] = '\0';
  switch (parser->previous_token_type) {
  case TOKEN_EQUALS:
  case TOKEN_PLUS:
  case TOKEN_MINUS:
  case TOKEN_MULTIPLY:
  case TOKEN_DIVIDE:
  case TOKEN_PERCENT:
  case TOKEN_AMPERSAND:
  case TOKEN_PIPE:
  case TOKEN_CARET:
  case TOKEN_LSHIFT:
  case TOKEN_RSHIFT:
  case TOKEN_AND_AND:
  case TOKEN_OR_OR:
  case TOKEN_EQUALS_EQUALS:
  case TOKEN_NOT_EQUALS:
  case TOKEN_LESS_THAN:
  case TOKEN_LESS_EQUALS:
  case TOKEN_GREATER_THAN:
  case TOKEN_GREATER_EQUALS:
  case TOKEN_NOT:
  case TOKEN_TILDE:
  case TOKEN_DOT_DOT:
  case TOKEN_COMMA:
  case TOKEN_LPAREN:
  case TOKEN_LBRACKET:
  case TOKEN_RETURN:
  case TOKEN_DISPATCH:
  case TOKEN_NEW:
    if (parser->previous_token_text[0] != '\0')
      snprintf(after, sizeof(after), " after '%s'",
               parser->previous_token_text);
    else
      snprintf(after, sizeof(after), " after %s",
               token_type_to_string(parser->previous_token_type));
    break;
  default:
    break;
  }

  char message[PARSER_ERROR_BUF_SIZE];
  snprintf(message, sizeof(message), "Expected an expression%s, found %s",
           after, found);

  // A suggestion aimed at the shape of the mistake, not at the grammar.
  char help[PARSER_ERROR_BUF_SIZE];
  const char *suggestion = NULL;
  TokenType found_type = parser->current_token.type;

  if (found_type == TOKEN_SEMICOLON || found_type == TOKEN_NEWLINE) {
    if (after[0] != '\0') {
      snprintf(help, sizeof(help), "write the value that belongs%s", after);
      suggestion = help;
    } else {
      suggestion = "the statement stops before it says anything; write a value";
    }
  } else if (found_type == TOKEN_EOF) {
    suggestion = "the file ends in the middle of an expression";
  } else if (found_type == TOKEN_RPAREN || found_type == TOKEN_RBRACKET ||
             found_type == TOKEN_RBRACE) {
    if (after[0] != '\0')
      snprintf(help, sizeof(help), "the value that belongs%s is missing", after);
    else
      snprintf(help, sizeof(help), "%s closes a group that holds no value",
               found);
    suggestion = help;
  } else if (found_type == TOKEN_COMMA) {
    suggestion = "an item is missing between the commas";
  } else if (found_type == TOKEN_EQUALS) {
    suggestion = "'=' assigns a value; to compare two values write '=='";
  } else if (parser_is_binary_operator(found_type)) {
    snprintf(help, sizeof(help), "'%s' needs a value on its left",
             parser->current_token.value ? parser->current_token.value : "?");
    suggestion = help;
  } else if (parser_token_starts_statement(found_type)) {
    snprintf(help, sizeof(help),
             "'%s' starts a statement and cannot stand in for a value",
             parser->current_token.value ? parser->current_token.value : "it");
    suggestion = help;
  }

  parser_set_error_with_suggestion(parser, message, suggestion);
  if (parser->error_reporter)
    error_reporter_set_last_label(parser->error_reporter,
                                  "expected an expression here");
}

/* String interpolation. "{expr}" inside a literal desugars right here, at the
 * token, into '+' concatenation over the split parts, so no later stage sees
 * interpolation as a distinct feature. Each expression part is wrapped in a
 * __mtl_interp() call; the type checker types that name and IR lowering
 * rewrites it to the matching mettle_string_from_* runtime conversion.
 *
 * Only '{' is special. '{{' is a literal '{'; '}' is a plain character except
 * while scanning for the end of an interpolation, where braces nest so struct
 * literals and blocks inside the expression survive. */
static void parser_interp_patch_locations(ASTNode *node,
                                          SourceLocation location) {
  if (!node)
    return;
  node->location = location;
  for (size_t i = 0; i < node->child_count; i++) {
    parser_interp_patch_locations(node->children[i], location);
  }
}

static ASTNode *parser_interp_append(ASTNode *chain, ASTNode *part,
                                     SourceLocation location) {
  if (!chain)
    return part;
  ASTNode *joined = ast_create_binary_expression(chain, "+", part, location);
  if (!joined) {
    ast_destroy_node(chain);
    ast_destroy_node(part);
  }
  return joined;
}

static ASTNode *parser_interp_parse_fragment(Parser *parser,
                                             const char *fragment,
                                             SourceLocation location) {
  Lexer *sub_lexer = lexer_create(fragment);
  Parser *sub_parser = sub_lexer ? parser_create(sub_lexer) : NULL;
  ASTNode *expr = sub_parser ? parser_parse_expression(sub_parser) : NULL;

  if (expr && sub_parser) {
    while (sub_parser->current_token.type == TOKEN_NEWLINE ||
           sub_parser->current_token.type == TOKEN_SEMICOLON) {
      parser_advance(sub_parser);
    }
    if (sub_parser->current_token.type != TOKEN_EOF) {
      ast_destroy_node(expr);
      expr = NULL;
    }
  }
  if (sub_parser && sub_parser->has_error && expr) {
    ast_destroy_node(expr);
    expr = NULL;
  }
  if (!expr) {
    char message[512];
    const char *detail =
        sub_parser && sub_parser->error_message ? sub_parser->error_message
                                                : "expected one expression";
    snprintf(message, sizeof(message),
             "Invalid expression in string interpolation '{%s}': %s", fragment,
             detail);
    parser_destroy(sub_parser);
    lexer_destroy(sub_lexer);
    parser_set_error(parser, message);
    return NULL;
  }
  parser_destroy(sub_parser);
  lexer_destroy(sub_lexer);
  parser_interp_patch_locations(expr, location);
  return expr;
}

static ASTNode *parser_parse_interpolated_string(Parser *parser,
                                                 const char *value,
                                                 size_t length,
                                                 SourceLocation location) {
  char *literal = malloc(length + 1);
  size_t literal_length = 0;
  ASTNode *chain = NULL;
  int has_expression_part = 0;
  size_t i = 0;

  if (!literal) {
    parser_set_error(parser, "Out of memory in string interpolation");
    return NULL;
  }

  while (i < length) {
    char c = value[i];
    if (c == '{' && i + 1 < length && value[i + 1] == '{') {
      literal[literal_length++] = '{';
      i += 2;
      continue;
    }
    if (c != '{') {
      literal[literal_length++] = c;
      i++;
      continue;
    }

    size_t start = ++i;
    int depth = 1;
    int in_nested = 0;
    while (i < length) {
      if (value[i] == '"') {
        in_nested = !in_nested;
      } else if (in_nested) {
        i++;
        continue;
      } else if (value[i] == '{') {
        depth++;
      } else if (value[i] == '}' && --depth == 0) {
        break;
      }
      i++;
    }
    if (depth != 0) {
      parser_set_error(parser,
                       "Unterminated '{' in string literal; write '{{' for a "
                       "literal brace");
      goto fail;
    }
    size_t expr_length = i - start;
    i++;
    if (expr_length == 0) {
      parser_set_error(parser, "Empty '{}' in string literal");
      goto fail;
    }

    if (literal_length > 0) {
      literal[literal_length] = '\0';
      ASTNode *part = ast_create_string_literal(literal, literal_length,
                                                location);
      if (!part)
        goto fail;
      chain = parser_interp_append(chain, part, location);
      if (!chain)
        goto fail;
      literal_length = 0;
    }

    char *fragment = malloc(expr_length + 1);
    if (!fragment) {
      parser_set_error(parser, "Out of memory in string interpolation");
      goto fail;
    }
    memcpy(fragment, value + start, expr_length);
    fragment[expr_length] = '\0';
    ASTNode *expr = parser_interp_parse_fragment(parser, fragment, location);
    free(fragment);
    if (!expr)
      goto fail;

    ASTNode *argument[1];
    argument[0] = expr;
    ASTNode *converted =
        ast_create_call_expression("__mtl_interp", argument, 1, location);
    if (!converted) {
      ast_destroy_node(expr);
      goto fail;
    }
    has_expression_part = 1;
    chain = parser_interp_append(chain, converted, location);
    if (!chain)
      goto fail;
  }

  literal[literal_length] = '\0';
  if (!has_expression_part) {
    ASTNode *only = ast_create_string_literal(literal, literal_length,
                                              location);
    free(literal);
    return only;
  }
  if (literal_length > 0) {
    ASTNode *part = ast_create_string_literal(literal, literal_length,
                                              location);
    if (!part)
      goto fail;
    chain = parser_interp_append(chain, part, location);
    if (!chain)
      goto fail;
  }
  free(literal);
  return chain;

fail:
  if (!parser->has_error) {
    parser_set_error(parser, "Out of memory in string interpolation");
  }
  ast_destroy_node(chain);
  free(literal);
  return NULL;
}

ASTNode *parser_parse_primary_expression(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);

  if (parser_is_identifier_like(parser->current_token.type) ||
      parser_is_type_keyword(parser->current_token.type)) {
    /* Type names (`int32`, `string`, ...) are compile-time Type values in
     * expression position, not a parallel type-level grammar. */
    ASTNode *result =
        ast_create_identifier(parser->current_token.value, location);
    parser_advance(parser);
    return result;
  }

  switch (parser->current_token.type) {
  case TOKEN_NUMBER: {
    // Check if it's a float or integer
    char *value = parser->current_token.value;
    ASTNode *result;

    // A decimal point or an exponent marker makes it a float. The radix prefix
    // has to be ruled out first: hex digits include 'e', so 0x1E is an integer
    // and only an unprefixed literal can carry an exponent.
    int has_radix_prefix =
        value[0] == '0' && (value[1] == 'x' || value[1] == 'X' ||
                            value[1] == 'b' || value[1] == 'B');
    int is_float_literal =
        strchr(value, '.') != NULL ||
        (!has_radix_prefix &&
         (strchr(value, 'e') != NULL || strchr(value, 'E') != NULL));

    if (is_float_literal) {
      double float_val = atof(value);
      result = ast_create_float_literal(float_val, location);
    } else {
      long long int_val = 0;
      unsigned char int_radix = 10;
      if (!parser_parse_integer_literal_string(parser, value, &int_val,
                                               &int_radix)) {
        return NULL;
      }
      result = ast_create_number_literal(int_val, location, int_radix);
      if (!result) {
        return NULL;
      }
      /* The lexer folded `'a'` to 97 and handed back a number. The lexeme
       * still opens with a quote, which is the only surviving trace that the
       * program wrote a character. */
      if (parser->current_token.lexeme.data &&
          parser->current_token.lexeme.length > 0 &&
          parser->current_token.lexeme.data[0] == '\'') {
        ((NumberLiteral *)result->data)->is_char = 1;
      }
    }

    parser_advance(parser);
    return result;
  }
  case TOKEN_STRING: {
    ASTNode *result;
    /* The lexeme length, not strlen: `\0` is a legal escape, so the literal's
     * bytes can run past an interior NUL and both the brace search and the
     * copy have to span all of them. */
    size_t token_length = parser->current_token.lexeme.length;
    if (parser->current_token.value &&
        memchr(parser->current_token.value, '{', token_length)) {
      result = parser_parse_interpolated_string(
          parser, parser->current_token.value, token_length, location);
    } else {
      result = ast_create_string_literal(parser->current_token.value,
                                         token_length, location);
    }
    parser_advance(parser);
    return result;
  }
  case TOKEN_IMPORT_STR: {
    parser_advance(parser); // consume 'import_str'
    if (parser->current_token.type != TOKEN_STRING) {
      parser_set_error(parser, "Expected string literal after 'import_str'");
      return NULL;
    }
    char *file_path = strdup(parser->current_token.value);
    parser_advance(parser); // consume the string
    ASTNode *node = ast_create_import_str(file_path, location);
    free(file_path);
    return node;
  }
  case TOKEN_LPAREN: {
    parser_advance(parser); // consume '('
    const char *saved_group = parser->group_context;
    parser->group_context = "grouped expression";
    ASTNode *expr = parser_parse_expression(parser);
    parser->group_context = "grouped expression";
    if (!parser_expect(parser, TOKEN_RPAREN)) {
      ast_destroy_node(expr);
      return NULL;
    }
    parser->group_context = saved_group;
    return expr;
  }
  /* Aggregate literals. A '[' only reaches primary position when nothing
   * precedes it (indexing is postfix), and a '{' only reaches an expression
   * when a block was not what was being parsed, so neither is ambiguous. */
  case TOKEN_LBRACKET:
    return parser_parse_array_literal(parser, location);
  case TOKEN_LBRACE:
    return parser_parse_struct_literal(parser, location);
  case TOKEN_FN:
  case TOKEN_FUNCTION:
    return parser_parse_lambda_expression(parser);
  case TOKEN_MATCH:
    return parser_parse_match_expression(parser);
  case TOKEN_THIS: {
    parser_advance(parser);
    return ast_create_identifier("this", location);
  }
  case TOKEN_NEW: {
    parser_advance(parser); // Built-in memory alloc handling
    if (!parser_is_identifier_like(parser->current_token.type) &&
        !parser_is_type_keyword(parser->current_token.type) &&
        parser->current_token.type != TOKEN_FN) {
      parser_set_error(parser, "Expected type name after 'new'");
      return NULL;
    }
    /* The array suffix is left unparsed: `new T[n]` allocates n of them, and
       the count is an expression the program computes. */
    char *type_name = parser_parse_type_annotation_ex(parser, 0);
    ASTNode *new_expr = NULL;
    if (!type_name) {
      return NULL;
    }

    if (parser->current_token.type == TOKEN_LBRACKET) {
      ASTNode *count = NULL;
      parser_advance(parser);
      count = parser_parse_expression(parser);
      if (!count) {
        free(type_name);
        return NULL;
      }
      new_expr = ast_create_new_array_expression(type_name, count, location);
      free(type_name);
      if (!new_expr) {
        ast_destroy_node(count);
        return NULL;
      }
      while (parser->current_token.type == TOKEN_COMMA) {
        ASTNode *extent = NULL;
        parser_advance(parser);
        extent = parser_parse_expression(parser);
        if (!extent || !ast_new_expression_add_extent(new_expr, extent)) {
          ast_destroy_node(extent);
          ast_destroy_node(new_expr);
          return NULL;
        }
      }
      if (!parser_expect(parser, TOKEN_RBRACKET)) {
        ast_destroy_node(new_expr);
        return NULL;
      }
      return new_expr;
    }

    new_expr = ast_create_new_expression(type_name, location);
    free(type_name);
    return new_expr;
  }
  default:
    parser_error_expected_expression(parser);
    return NULL;
  }
}

static ParserSavedState parser_save_state(Parser *parser) {
  ParserSavedState state;
  state.lexer_position = parser->lexer->position;
  state.lexer_line = parser->lexer->line;
  state.lexer_column = parser->lexer->column;
  state.lexer_continuation_depth = parser->lexer->continuation_depth;
  state.current_token = token_clone(&parser->current_token);
  state.peek_token = token_clone(&parser->peek_token);
  state.has_error = parser->has_error;
  state.had_error = parser->had_error;
  state.error_count = parser->error_count;
  state.error_message =
      parser->error_message ? strdup(parser->error_message) : NULL;
  state.error_reporter = parser->error_reporter;
  return state;
}

static void parser_restore_state(Parser *parser,
                                 const ParserSavedState *state) {
  parser->lexer->position = state->lexer_position;
  parser->lexer->line = state->lexer_line;
  parser->lexer->column = state->lexer_column;
  parser->lexer->continuation_depth = state->lexer_continuation_depth;

  token_destroy(&parser->current_token);
  parser->current_token = token_clone(&state->current_token);
  token_destroy(&parser->peek_token);
  parser->peek_token = token_clone(&state->peek_token);

  parser->has_error = state->has_error;
  parser->had_error = state->had_error;
  parser->error_count = state->error_count;
  parser->error_reporter = state->error_reporter;
  free(parser->error_message);
  parser->error_message =
      state->error_message ? strdup(state->error_message) : NULL;
}

static void parser_discard_saved_state(ParserSavedState *state) {
  token_destroy(&state->current_token);
  token_destroy(&state->peek_token);
  free(state->error_message);
}

ASTNode *parser_parse_cast_expression(Parser *parser) {
  if (!parser || parser->current_token.type != TOKEN_LPAREN)
    return NULL;

  // Only try if the next token could start a type name
  if (!parser_is_type_keyword(parser->peek_token.type) &&
      !parser_is_identifier_like(parser->peek_token.type) &&
      parser->peek_token.type != TOKEN_FN) {
    return NULL;
  }

  ParserSavedState saved = parser_save_state(parser);
  parser->error_message = NULL;
  parser->error_reporter = NULL;

  SourceLocation location = parser_current_location(parser);

  parser_advance(parser); // consume '('

  // Remember whether the parenthesized type begins with a built-in type
  // keyword (int32, int64, float64, ...). The parser keeps no registry of
  // user-defined type names, so this is its only reliable signal that the
  // parenthesized token is genuinely a type rather than a value. We use it
  // below to disambiguate `(name) <op> x` where <op> is both a unary and a
  // binary operator (`&`, `*`, `+`, `-`).
  int type_starts_with_keyword =
      parser_is_type_keyword(parser->current_token.type) ||
      parser_is_builtin_type_name(parser->current_token.value);

  char *type_name = parser_parse_type_annotation(parser);
  if (!type_name || parser->has_error ||
      parser->current_token.type != TOKEN_RPAREN) {
    free(type_name);
    parser_restore_state(parser, &saved);
    parser_discard_saved_state(&saved);
    return NULL;
  }

  parser->error_reporter = saved.error_reporter;

  parser_advance(parser); // consume ')'

  // A bare single identifier (no built-in keyword, and no pointer/array/
  // generic/qualified structure such as `*`, `[`, `<` or `.`) gives the parser
  // no reason to believe it names a type. `(MyStruct*)`, `(Vec<T>)`,
  // `(mod.Type)` and the like are structurally types and stay casts.
  int looks_like_type =
      type_starts_with_keyword || strpbrk(type_name, "*[<.(") != NULL;

  // Check if it's a grouped expression instead
  int is_binary = parser_is_binary_operator(parser->current_token.type);
  int is_unary = parser_is_unary_operator(parser->current_token.type);
  if (parser->current_token.type == TOKEN_RPAREN ||
      parser->current_token.type == TOKEN_COMMA ||
      parser->current_token.type == TOKEN_SEMICOLON ||
      parser->current_token.type == TOKEN_EOF ||
      (is_binary && !is_unary) ||
      // Ambiguous prefix operator (`&`/`*`/`+`/`-`) after a token that does not
      // look like a type: treat `(name) <op> x` as a parenthesized expression
      // followed by a binary operator, not a cast of a unary expression.
      (is_binary && is_unary && !looks_like_type)) {
    free(type_name);
    parser_restore_state(parser, &saved);
    parser_discard_saved_state(&saved);
    return NULL;
  }

  parser_discard_saved_state(&saved);
  parser->has_error = 0;
  free(parser->error_message);
  parser->error_message = NULL;

  ASTNode *operand = parser_parse_unary_expression(parser);
  if (!operand) {
    free(type_name);
    return NULL;
  }

  ASTNode *cast_expr = ast_create_cast_expression(type_name, operand, location);
  free(type_name);
  return cast_expr;
}

ASTNode *parser_parse_unary_expression(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);

  /* A run of prefix operators, and a run of casts, each recurse here without
   * passing through parser_parse_expression, so they answer to the ceiling
   * themselves. */
  if (parser->expression_depth >= PARSER_MAX_EXPRESSION_DEPTH) {
    parser_report_expression_too_deep(parser);
    return NULL;
  }

  if (parser->current_token.type == TOKEN_LPAREN) {
    parser->expression_depth++;
    ASTNode *cast = parser_parse_cast_expression(parser);
    parser->expression_depth--;
    if (cast) {
      return cast;
    }
  }

  if (parser_is_unary_operator(parser->current_token.type)) {
    const char *operator = parser->current_token.value;
    parser_advance(parser);

    parser->expression_depth++;
    ASTNode *operand = parser_parse_unary_expression(parser);
    parser->expression_depth--;
    if (!operand) {
      return NULL;
    }

    return ast_create_unary_expression(operator, operand, location);
  }

  return parser_parse_postfix_expression(parser);
}

static int parser_try_parse_generic_call_type_args(Parser *parser,
                                                   char ***out_type_args,
                                                   size_t *out_type_arg_count) {
  *out_type_args = NULL;
  *out_type_arg_count = 0;

  if (parser->current_token.type != TOKEN_LESS_THAN)
    return 0;

  ParserSavedState saved = parser_save_state(parser);
  parser->error_message = NULL;
  // This is a speculative parse: `<` after an identifier is far more often a
  // comparison than a type-argument list. Detach the reporter first, exactly as
  // parser_parse_cast_expression does, so a failed speculation leaves no
  // diagnostic behind - parser_restore_state alone cannot retract one that has
  // already been handed to the reporter. Without this, `v < fields[i].range.lo`
  // backtracks and reparses correctly but still fails the build with the
  // speculation's "Expected array size after '['".
  parser->error_reporter = NULL;

  parser_advance(parser); // consume '<'

  char **type_args = NULL;
  size_t type_arg_count = 0;
  int success = 1;

  while (!parser_at_type_arg_close(parser) &&
         parser->current_token.type != TOKEN_EOF) {
    if (type_arg_count > 0) {
      if (parser->current_token.type != TOKEN_COMMA) {
        success = 0;
        break;
      }
      parser_advance(parser);
    }

    char *arg = parser_parse_type_annotation(parser);
    if (!arg || parser->has_error) {
      free(arg);
      success = 0;
      break;
    }

    type_args = realloc(type_args, (type_arg_count + 1) * sizeof(char *));
    type_args[type_arg_count++] = arg;
  }

  if (success && parser_consume_type_arg_close(parser)) {
    if (parser->current_token.type == TOKEN_LPAREN) {
      *out_type_args = type_args;
      *out_type_arg_count = type_arg_count;
      parser->error_reporter = saved.error_reporter;
      parser_discard_saved_state(&saved);
      parser->has_error = 0;
      free(parser->error_message);
      parser->error_message = NULL;
      return 1;
    }
  }

  for (size_t i = 0; i < type_arg_count; i++)
    free(type_args[i]);
  free(type_args);

  parser_restore_state(parser, &saved);
  parser_discard_saved_state(&saved);

  return 0;
}

// Kernel index built-ins: maps `<obj>.<axis>` to its target-neutral GPU index
// intrinsic link-name (gpu_tid_x etc.), or NULL if not a built-in:
//   thread.x     -> gpu_tid_x
//   block.x      -> gpu_ctaid_x
//   block_dim.x  -> gpu_ntid_x
//   grid_dim.x   -> gpu_nctaid_x
// Only consulted in GPU-module (gpu_mode) compiles. Returns a pointer into a
// static buffer (single-threaded parse; copied immediately by the caller).
static const char *parser_gpu_index_intrinsic(const char *obj,
                                              const char *axis) {
  if (!obj || !axis || axis[0] == 0 || axis[1] != 0) {
    return NULL;
  }
  char a = axis[0];
  if (a != 'x' && a != 'y' && a != 'z') {
    return NULL;
  }
  const char *base = NULL;
  if (strcmp(obj, "thread") == 0) {
    base = "gpu_tid_";
  } else if (strcmp(obj, "block") == 0) {
    base = "gpu_ctaid_";
  } else if (strcmp(obj, "block_dim") == 0) {
    base = "gpu_ntid_";
  } else if (strcmp(obj, "grid_dim") == 0) {
    base = "gpu_nctaid_";
  } else {
    return NULL;
  }
  static char buf[24];
  snprintf(buf, sizeof(buf), "%s%c", base, a);
  return buf;
}

ASTNode *parser_parse_postfix_expression(Parser *parser) {
  if (!parser)
    return NULL;

  ASTNode *expr = parser_parse_primary_expression(parser);
  if (!expr)
    return NULL;

  while (1) {
    /* A line opening with `.` or `->` continues this one, so a chain can be
     * written down the page. Neither can begin a statement, so nothing that
     * used to parse as two statements now parses as one. */
    parser_skip_chain_continuation(parser);
    SourceLocation location = parser_current_location(parser);

    if (expr->type == AST_IDENTIFIER &&
        parser->current_token.type == TOKEN_LESS_THAN) {
      char **call_type_args = NULL;
      size_t call_type_arg_count = 0;
      if (parser_try_parse_generic_call_type_args(parser, &call_type_args,
                                                  &call_type_arg_count)) {
        parser_advance(parser); // consume '('
        const char *saved_group = parser->group_context;
        parser->group_context = "argument list";

        ASTNode **arguments = NULL;
        size_t arg_count = 0;

        if (parser->current_token.type != TOKEN_RPAREN) {
          do {
            parser->group_context = "argument list";
            ASTNode *arg = parser_parse_expression(parser);
            if (!arg)
              break;
            arguments = realloc(arguments, (arg_count + 1) * sizeof(ASTNode *));
            arguments[arg_count++] = arg;
            if (parser->current_token.type == TOKEN_COMMA) {
              parser_advance(parser);
            } else if (parser->current_token.type == TOKEN_RPAREN) {
              break;
            } else {
              parser_set_error(parser, "Expected ',' or ')' in argument list");
              break;
            }
          } while (1);
        }
        parser->group_context = "argument list";

        if (!parser_expect(parser, TOKEN_RPAREN)) {
          for (size_t i = 0; i < arg_count; i++)
            ast_destroy_node(arguments[i]);
          free(arguments);
          for (size_t i = 0; i < call_type_arg_count; i++)
            free(call_type_args[i]);
          free(call_type_args);
          ast_destroy_node(expr);
          return NULL;
        }
        parser->group_context = saved_group;

        Identifier *id_data = (Identifier *)expr->data;
        char *func_name = strdup(id_data->name);
        ast_destroy_node(expr);

        expr = ast_create_call_expression(func_name, arguments, arg_count,
                                          location);
        if (expr && expr->data) {
          CallExpression *ce = (CallExpression *)expr->data;
          ce->type_args = call_type_args;
          ce->type_arg_count = call_type_arg_count;
        } else {
          for (size_t i = 0; i < call_type_arg_count; i++)
            free(call_type_args[i]);
          free(call_type_args);
        }
        free(func_name);
        continue;
      }
    }

    if (parser->current_token.type == TOKEN_LPAREN) {
      // Function call or method call
      int tensor_named_call =
          parser_identifier_name_is(expr, "tensor_mma") ||
          parser_identifier_name_is(expr, "tensor_matmul") ||
          parser_identifier_name_is(expr, "tensor_epilogue") ||
          parser_identifier_name_is(expr, "tensor_transfer_workgroup");
      int atomic_named_call =
          parser_identifier_name_is(expr, "atomic_fetch_add") ||
          parser_identifier_name_is(expr, "atomic_fetch_sub") ||
          parser_identifier_name_is(expr, "atomic_fetch_min") ||
          parser_identifier_name_is(expr, "atomic_fetch_max") ||
          parser_identifier_name_is(expr, "atomic_fetch_and") ||
          parser_identifier_name_is(expr, "atomic_fetch_or") ||
          parser_identifier_name_is(expr, "atomic_fetch_xor") ||
          parser_identifier_name_is(expr, "atomic_load") ||
          parser_identifier_name_is(expr, "atomic_store") ||
          parser_identifier_name_is(expr, "atomic_exchange") ||
          parser_identifier_name_is(expr, "atomic_compare_exchange");
      int async_named_call =
          parser_identifier_name_is(expr, "async_copy_workgroup");
      int compiler_named_call =
          tensor_named_call || atomic_named_call || async_named_call;
      parser_advance(parser); // consume '('

      // Parse arguments first (common to both cases)
      ASTNode **arguments = NULL;
      char **argument_names = NULL;
      size_t arg_count = 0;

      /* Set when typeof's argument was read as a type, so the ordinary
       * argument parse below knows the list is already complete. */
      int typeof_type_argument = 0;
      /* `typeof` takes either a type or an expression, and only one of those
       * is spelled with `*` or `[N]`. Try the type reading first and fall back
       * to the expression parse, so `typeof(Point*)` and `typeof(n)` both
       * work without the grammar having to tell them apart up front. */
      if (parser_identifier_name_is(expr, "typeof") &&
          parser->current_token.type != TOKEN_RPAREN) {
        ParserSavedState typeof_saved = parser_save_state(parser);
        SourceLocation type_location = parser_current_location(parser);
        parser->error_message = NULL;
        parser->error_reporter = NULL;

        char *type_name = parser_parse_type_annotation(parser);
        int is_type = type_name && !parser->has_error &&
                      parser->current_token.type == TOKEN_RPAREN &&
                      (strchr(type_name, '*') || strchr(type_name, '['));

        parser->error_reporter = typeof_saved.error_reporter;
        if (!is_type) {
          free(type_name);
          parser_restore_state(parser, &typeof_saved);
          parser_discard_saved_state(&typeof_saved);
        } else {
          parser_discard_saved_state(&typeof_saved);
          ASTNode *type_arg = ast_create_identifier(type_name, type_location);
          free(type_name);
          arguments = malloc(sizeof(ASTNode *));
          if (!type_arg || !arguments) {
            ast_destroy_node(type_arg);
            free(arguments);
            ast_destroy_node(expr);
            parser_set_error(parser, "Out of memory parsing typeof");
            return NULL;
          }
          arguments[0] = type_arg;
          arg_count = 1;
          typeof_type_argument = 1;
        }
      }

      if (parser_identifier_name_is(expr, "sizeof")) {
        SourceLocation type_location = parser_current_location(parser);
        if (parser->current_token.type == TOKEN_RPAREN) {
          parser_set_error(parser, "Expected type name in sizeof");
          ast_destroy_node(expr);
          return NULL;
        }

        char *type_name = parser_parse_type_annotation(parser);
        if (!type_name) {
          if (!parser->has_error) {
            parser_set_error(parser, "Expected type name in sizeof");
          }
          ast_destroy_node(expr);
          return NULL;
        }

        ASTNode *type_arg = ast_create_identifier(type_name, type_location);
        free(type_name);
        if (!type_arg) {
          ast_destroy_node(expr);
          return NULL;
        }

        arguments = malloc(sizeof(ASTNode *));
        if (!arguments) {
          ast_destroy_node(type_arg);
          ast_destroy_node(expr);
          parser_set_error(parser, "Out of memory parsing sizeof");
          return NULL;
        }
        arguments[0] = type_arg;
        arg_count = 1;

        if (parser->current_token.type == TOKEN_COMMA) {
          parser_set_error(parser, "sizeof expects exactly one type argument");
          ast_destroy_node(type_arg);
          free(arguments);
          ast_destroy_node(expr);
          return NULL;
        }
      } else if (!typeof_type_argument &&
                 parser->current_token.type != TOKEN_RPAREN) {
        do {
          char *argument_name = NULL;
          if (compiler_named_call &&
              parser->current_token.type == TOKEN_IDENTIFIER &&
              parser->peek_token.type == TOKEN_COLON) {
            argument_name = strdup(parser->current_token.value);
            parser_advance(parser); /* name -> ':' */
            parser_advance(parser); /* ':' -> value */
          }
          ASTNode *arg = NULL;
          /* `workgroup` is a declaration keyword, but it is also the neutral
           * memory-model spelling used by native atomic named options. Preserve
           * it as an identifier value in this tightly scoped call grammar. */
          if (compiler_named_call && argument_name &&
              parser->current_token.type == TOKEN_WORKGROUP) {
            arg = ast_create_identifier(
                "workgroup", parser_current_location(parser));
            parser_advance(parser);
          } else {
            arg = parser_parse_expression(parser);
          }
          if (!arg) {
            free(argument_name);
            break;
          }

          arguments = realloc(arguments, (arg_count + 1) * sizeof(ASTNode *));
          argument_names =
              realloc(argument_names, (arg_count + 1) * sizeof(char *));
          arguments[arg_count] = arg;
          argument_names[arg_count] = argument_name;
          arg_count++;

          if (parser->current_token.type == TOKEN_COMMA) {
            parser_advance(parser);
          } else if (parser->current_token.type == TOKEN_RPAREN) {
            break;
          } else {
            parser_set_error(parser, "Expected ',' or ')' in argument list");
            break;
          }
        } while (1);
      }
      parser->group_context = "argument list";

      if (!parser_expect(parser, TOKEN_RPAREN)) {
        for (size_t i = 0; i < arg_count; i++) {
          ast_destroy_node(arguments[i]);
          free(argument_names ? argument_names[i] : NULL);
        }
        free(arguments);
        free(argument_names);
        ast_destroy_node(expr);
        return NULL;
      }
      parser->group_context = NULL;

      if (expr->type == AST_MEMBER_ACCESS) {
        // Method call: obj.method(args)
        MemberAccess *access = (MemberAccess *)expr->data;
        char *method_name = strdup(access->member);
        ASTNode *object = access->object;
        // Detach the object from the member access so it's not double-freed
        access->object = NULL;
        free(expr->children);
        expr->children = NULL;
        expr->child_count = 0;
        ast_destroy_node(expr);

        expr = ast_create_method_call(object, method_name, arguments, arg_count,
                                      location);
        for (size_t i = 0; i < arg_count; i++)
          free(argument_names ? argument_names[i] : NULL);
        free(argument_names);
        free(method_name);
      } else if (expr->type == AST_IDENTIFIER) {
        // Regular function call (or function pointer variable - type checker
        // validates)
        Identifier *id_data = (Identifier *)expr->data;
        char *func_name = strdup(id_data->name);
        ast_destroy_node(expr);

        expr = ast_create_call_expression(func_name, arguments, arg_count,
                                          location);
        if (expr && expr->data) {
          ((CallExpression *)expr->data)->argument_names = argument_names;
          argument_names = NULL;
        }
        for (size_t i = 0; i < arg_count && argument_names; i++)
          free(argument_names[i]);
        free(argument_names);
        free(func_name);
      } else {
        // (expr)(args) / f(...)(args): call through the value the expression
        // produces - a thin function pointer or a closure. The node owns the
        // callee expression and the argument nodes; only the array is ours.
        ASTNode *fp = ast_create_func_ptr_call(expr, arguments, arg_count,
                                               location);
        for (size_t i = 0; i < arg_count; i++)
          free(argument_names ? argument_names[i] : NULL);
        free(argument_names);
        free(arguments);
        if (!fp) {
          return NULL;
        }
        expr = fp;
      }

    } else if (parser->current_token.type == TOKEN_DOT) {
      // Member access
      parser_advance(parser); // consume '.'

      /* A field name here is subject to the same rule as where it was declared
       * and where a literal initializes it, both of which accept any
       * identifier-like token. The x86 mnemonics are lexed as keywords
       * everywhere, not just inside an `asm` block, so requiring a bare
       * TOKEN_IDENTIFIER meant a field called `add`, `sub`, `cmp`, `div`, or
       * `mov` could be declared and initialized but never read back. */
      if (!parser_is_identifier_like(parser->current_token.type)) {
        parser_set_error(parser, "Expected member name after '.'");
        ast_destroy_node(expr);
        return NULL;
      }

      char *member = strdup(parser->current_token.value);
      parser_advance(parser);

      // GPU kernel index built-ins: `thread.x` etc. desugar to a call to the
      // corresponding target-neutral gpu_* intrinsic in GPU-module compiles.
      const char *gpu_intr = NULL;
      if (parser->gpu_mode && expr->type == AST_IDENTIFIER && expr->data) {
        gpu_intr =
            parser_gpu_index_intrinsic(((Identifier *)expr->data)->name, member);
      }
      if (gpu_intr) {
        ASTNode *call = ast_create_call_expression(gpu_intr, NULL, 0, location);
        ast_destroy_node(expr);
        free(member);
        if (!call) {
          return NULL;
        }
        ((CallExpression *)call->data)->is_gpu_index = 1;
        expr = call;
      } else {
        expr = ast_create_member_access(expr, member, location);
        free(member);
      }

    } else if (parser->current_token.type == TOKEN_ARROW) {
      // Pointer member access: p->field == (*p).field
      parser_advance(parser); // consume '->'

      if (!parser_is_identifier_like(parser->current_token.type)) {
        parser_set_error(parser, "Expected member name after '->'");
        ast_destroy_node(expr);
        return NULL;
      }

      char *member = strdup(parser->current_token.value);
      parser_advance(parser);

      ASTNode *deref = ast_create_unary_expression("*", expr, location);
      if (!deref) {
        free(member);
        ast_destroy_node(expr);
        return NULL;
      }

      expr = ast_create_member_access(deref, member, location);
      free(member);

    } else if (parser->current_token.type == TOKEN_LBRACKET) {
      parser_advance(parser); // consume '['

      ASTNode *index_expr = parser_parse_expression(parser);
      if (!index_expr) {
        ast_destroy_node(expr);
        return NULL;
      }

      if (!parser_expect(parser, TOKEN_RBRACKET)) {
        ast_destroy_node(index_expr);
        ast_destroy_node(expr);
        return NULL;
      }

      expr = ast_create_array_index_expression(expr, index_expr, location);

    } else {
      break;
    }
  }

  return expr;
}

ASTNode *parser_parse_binary_expression(Parser *parser, int min_precedence) {
  if (!parser)
    return NULL;

  ASTNode *left = parser_parse_unary_expression(parser);
  if (!left)
    return NULL;

  /* Operators of one precedence fold left here rather than recursing, so this
   * loop deepens the tree without deepening the parser. The later passes walk
   * that tree recursively, so the fold has to answer to the same ceiling; the
   * count is restored on the way out so a sibling expression starts level. */
  int enclosing_depth = parser->expression_depth;

  for (;;) {
    /* Blank lines inside a split expression: stepping over one leaves another
     * newline current, so a statement that does end here still sees one. */
    while (parser->current_token.type == TOKEN_NEWLINE &&
           parser->peek_token.type == TOKEN_NEWLINE) {
      parser_advance(parser);
    }
    if (parser->current_token.type == TOKEN_NEWLINE &&
        parser_operator_opens_continuation_line(parser->peek_token.type) &&
        parser_get_operator_precedence(parser->peek_token.type) >=
            min_precedence) {
      parser_advance(parser); // the operator opens the next line
    }
    if (!parser_is_binary_operator(parser->current_token.type)) {
      break;
    }
    SourceLocation location = parser_current_location(parser);
    int precedence = parser_get_operator_precedence(parser->current_token.type);
    if (precedence < min_precedence)
      break;

    if (parser->expression_depth >= PARSER_MAX_EXPRESSION_DEPTH) {
      parser->expression_depth = enclosing_depth;
      ast_destroy_node(left);
      parser_report_expression_too_deep(parser);
      return NULL;
    }
    parser->expression_depth++;

    char *operator = strdup(parser->current_token.value);
    parser_advance(parser);

    while (parser->current_token.type == TOKEN_NEWLINE) {
      parser_advance(parser);
    }

    ASTNode *right = parser_parse_binary_expression(parser, precedence + 1);
    if (!right) {
      free(operator);
      ast_destroy_node(left);
      parser->expression_depth = enclosing_depth;
      return NULL;
    }

    left = ast_create_binary_expression(left, operator, right, location);
    free(operator);
  }

  parser->expression_depth = enclosing_depth;
  return left;
}

// Parse an optional platform guard on an import: `import "..." if windows;`
// (or `if linux`). On success writes a heap-allocated platform name to
// *out_guard (or NULL when there is no guard) and returns 1; returns 0 on error.
static int parser_parse_import_guard(Parser *parser, char **out_guard) {
  *out_guard = NULL;
  if (parser->current_token.type != TOKEN_IF) {
    return 1;
  }
  parser_advance(parser); // consume 'if'
  if (!parser_is_identifier_like(parser->current_token.type) ||
      !parser->current_token.value) {
    parser_set_error(parser,
                     "Expected platform name after 'if' in import guard");
    return 0;
  }
  const char *name = parser->current_token.value;
  if (strcmp(name, "windows") != 0 && strcmp(name, "linux") != 0) {
    parser_set_error(parser,
                     "Import guard platform must be 'windows' or 'linux'");
    return 0;
  }
  *out_guard = strdup(name);
  parser_advance(parser); // consume platform name
  if (!*out_guard) {
    parser_set_error(parser, "Out of memory parsing import guard");
    return 0;
  }
  return 1;
}

ASTNode *parser_parse_import_declaration(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  parser_advance(parser); // consume 'import'

  // Selective import: import { a, b } from "mod"
  if (parser->current_token.type == TOKEN_LBRACE) {
    parser_advance(parser); // consume '{'

    char **selected = NULL;
    size_t selected_count = 0;
    size_t selected_capacity = 0;

    while (parser->current_token.type != TOKEN_RBRACE &&
           parser->current_token.type != TOKEN_EOF) {
      if (!parser_is_identifier_like(parser->current_token.type)) {
        parser_set_error(parser, "Expected identifier in import list");
        for (size_t i = 0; i < selected_count; i++) free(selected[i]);
        free(selected);
        return NULL;
      }

      if (selected_count >= selected_capacity) {
        size_t new_cap = selected_capacity == 0 ? 8 : selected_capacity * 2;
        char **grown = realloc(selected, new_cap * sizeof(char *));
        if (!grown) {
          for (size_t i = 0; i < selected_count; i++) free(selected[i]);
          free(selected);
          parser_set_error(parser, "Out of memory parsing import list");
          return NULL;
        }
        selected = grown;
        selected_capacity = new_cap;
      }

      selected[selected_count++] = strdup(parser->current_token.value);
      parser_advance(parser); // consume name

      if (parser->current_token.type == TOKEN_COMMA) {
        parser_advance(parser); // consume ','
      } else {
        break;
      }
    }

    if (parser->current_token.type != TOKEN_RBRACE) {
      parser_set_error(parser, "Expected '}' after import list");
      for (size_t i = 0; i < selected_count; i++) free(selected[i]);
      free(selected);
      return NULL;
    }
    parser_advance(parser); // consume '}'

    // expect 'from'
    if (!parser_is_identifier_like(parser->current_token.type) ||
        !parser->current_token.value ||
        strcmp(parser->current_token.value, "from") != 0) {
      parser_set_error(parser, "Expected 'from' after import list");
      for (size_t i = 0; i < selected_count; i++) free(selected[i]);
      free(selected);
      return NULL;
    }
    parser_advance(parser); // consume 'from'

    if (parser->current_token.type != TOKEN_STRING) {
      parser_set_error(parser, "Expected string literal after 'from'");
      for (size_t i = 0; i < selected_count; i++) free(selected[i]);
      free(selected);
      return NULL;
    }

    char *module_name = strdup(parser->current_token.value);
    parser_advance(parser); // consume string

    char *guard = NULL;
    if (!parser_parse_import_guard(parser, &guard)) {
      free(module_name);
      for (size_t i = 0; i < selected_count; i++) free(selected[i]);
      free(selected);
      return NULL;
    }

    if (!parser_expect_statement_end(parser)) {
      free(guard);
      free(module_name);
      for (size_t i = 0; i < selected_count; i++) free(selected[i]);
      free(selected);
      return NULL;
    }

    ASTNode *node = ast_create_import_declaration(
        module_name, NULL, (const char **)selected, selected_count, location);
    if (node && node->data && guard) {
      ((ImportDeclaration *)node->data)->platform_guard = guard; // transfer
    } else {
      free(guard);
    }
    free(module_name);
    for (size_t i = 0; i < selected_count; i++) free(selected[i]);
    free(selected);
    return node;
  }

  // Plain import or namespaced import
  if (parser->current_token.type != TOKEN_STRING) {
    parser_set_error(parser, "Expected string literal after 'import'");
    return NULL;
  }

  char *module_name = strdup(parser->current_token.value);
  char *namespace_alias = NULL;
  parser_advance(parser); // consume string

  if (parser_is_identifier_like(parser->current_token.type) &&
      parser->current_token.value &&
      strcmp(parser->current_token.value, "as") == 0) {
    parser_advance(parser); // consume 'as'
    if (!parser_is_identifier_like(parser->current_token.type)) {
      free(module_name);
      parser_set_error(parser, "Expected namespace alias after 'as'");
      return NULL;
    }
    namespace_alias = strdup(parser->current_token.value);
    parser_advance(parser); // consume alias
  }

  char *guard = NULL;
  if (!parser_parse_import_guard(parser, &guard)) {
    free(module_name);
    free(namespace_alias);
    return NULL;
  }

  if (!parser_expect_statement_end(parser)) {
    free(guard);
    free(module_name);
    free(namespace_alias);
    return NULL;
  }

  ASTNode *node =
      ast_create_import_declaration(module_name, namespace_alias, NULL, 0, location);
  if (node && node->data && guard) {
    ((ImportDeclaration *)node->data)->platform_guard = guard; // transfer
  } else {
    free(guard);
  }
  free(module_name);
  free(namespace_alias);

  return node;
}

static ASTNode *parser_parse_extern_var_declaration(Parser *parser) {
  if (!parser) {
    return NULL;
  }

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, TOKEN_VAR)) {
    return NULL;
  }

  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected variable name after 'extern var'");
    return NULL;
  }

  char *var_name = strdup(parser->current_token.value);
  parser_advance(parser);
  if (!var_name) {
    parser_set_error(parser, "Memory allocation failed");
    return NULL;
  }

  if (parser->current_token.type != TOKEN_COLON) {
    parser_set_error(parser,
                     "Extern variable declarations require an explicit type");
    free(var_name);
    return NULL;
  }
  parser_advance(parser); // consume ':'

  char *type_name = parser_parse_type_annotation(parser);
  if (!type_name) {
    free(var_name);
    parser_set_error(parser, "Extern variable declarations require a type");
    return NULL;
  }

  char *link_name = NULL;
  if (parser->current_token.type == TOKEN_EQUALS) {
    parser_advance(parser); // consume '='
    if (parser->current_token.type != TOKEN_STRING) {
      parser_set_error(
          parser,
          "Extern variable declarations cannot have an initializer; expected "
          "string literal link name after '='");
      free(var_name);
      free(type_name);
      return NULL;
    }
    link_name = strdup(parser->current_token.value);
    parser_advance(parser);
    if (!link_name) {
      parser_set_error(parser, "Memory allocation failed for link name");
      free(var_name);
      free(type_name);
      return NULL;
    }
  }

  if (!parser_expect_statement_end(parser)) {
    free(var_name);
    free(type_name);
    free(link_name);
    return NULL;
  }

  ASTNode *var_decl =
      ast_create_var_declaration(var_name, type_name, NULL, location);
  free(var_name);
  free(type_name);
  if (!var_decl || !var_decl->data) {
    free(link_name);
    return var_decl;
  }

  VarDeclaration *var_data = (VarDeclaration *)var_decl->data;
  var_data->is_extern = 1;
  if (link_name) {
    var_data->link_name = strdup(link_name);
    if (!var_data->link_name) {
      free(link_name);
      ast_destroy_node(var_decl);
      parser_set_error(parser, "Memory allocation failed for link name");
      return NULL;
    }
  }
  free(link_name);

  return var_decl;
}

ASTNode *parser_parse_var_declaration(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  AstAddressSpace address_space = AST_ADDRESS_SPACE_DEFAULT;
  if (parser->current_token.type == TOKEN_WORKGROUP ||
      parser->current_token.type == TOKEN_PRIVATE) {
    address_space = parser->current_token.type == TOKEN_WORKGROUP
                        ? AST_ADDRESS_SPACE_WORKGROUP
                        : AST_ADDRESS_SPACE_PRIVATE;
    parser_advance(parser);
    if (parser->current_token.type != TOKEN_VAR) {
      parser_set_error(parser,
                       "Expected 'var' after GPU address-space qualifier");
      return NULL;
    }
  }
  // Expect 'var' or 'const' keyword
  int is_const = (parser->current_token.type == TOKEN_CONST);
  if (is_const) {
    parser_advance(parser); // consume 'const'
  } else if (!parser_expect(parser, TOKEN_VAR)) {
    return NULL;
  }

  // Expect identifier
  char *var_name = NULL;
  ASTNode *composed_name = NULL;
  if (!parser_parse_declaration_name(parser,
                                     is_const
                                         ? "Expected identifier after 'const'"
                                         : "Expected identifier after 'var'",
                                     &var_name, &composed_name)) {
    return NULL;
  }

  char *type_name = NULL;
  ASTNode *initializer = NULL;

  // Optional type annotation: ': type'
  if (parser->current_token.type == TOKEN_COLON) {
    parser_advance(parser); // consume ':'

    type_name = parser_parse_type_annotation(parser);
    if (!type_name) {
      if (!parser->has_error) {
        parser_set_error(parser, "Expected type after ':'");
      }
      free(var_name);
      return NULL;
    }
  }

  // Optional initializer: '= expression'
  if (parser->current_token.type == TOKEN_EQUALS) {
    parser_advance(parser); // consume '='

    initializer = parser_parse_expression(parser);
    if (!initializer) {
      free(var_name);
      free(type_name);
      return NULL;
    }
  }

  // A constant must always have a value to fold at compile time.
  if (is_const && !initializer) {
    parser_set_error(parser, "Constant declaration requires an initializer");
    free(var_name);
    free(type_name);
    return NULL;
  }

  // For type inference, if no type is specified but there's an initializer,
  // we'll leave type_name as NULL and let the semantic analyzer infer it
  if (!type_name && !initializer) {
    parser_set_error(parser, "Variable declaration must have either a type "
                             "annotation or an initializer");
    free(var_name);
    return NULL;
  }

  // Expect a newline or semicolon to end the declaration
  parser_expect_statement_end(parser);

  ASTNode *var_decl =
      ast_create_var_declaration(var_name, type_name, initializer, location);
  if (var_decl && var_decl->data) {
    VarDeclaration *data = (VarDeclaration *)var_decl->data;
    data->is_const = is_const;
    data->address_space = address_space;
    data->composed_name = composed_name;
    composed_name = NULL;
    parser->pending_composed_name = NULL;
  }
  ast_destroy_node(composed_name);

  free(var_name);
  free(type_name);

  return var_decl;
}

ASTNode *parser_parse_barrier_statement(Parser *parser) {
  if (!parser || parser->current_token.type != TOKEN_BARRIER) return NULL;
  SourceLocation location = parser_current_location(parser);
  unsigned regions = 0;
  AstMemoryOrder order = AST_MEMORY_ORDER_SEQ_CST;
  int saw_order = 0;
  parser_advance(parser);
  if (!parser_expect(parser, TOKEN_LPAREN)) return NULL;
  while (parser->current_token.type != TOKEN_RPAREN &&
         parser->current_token.type != TOKEN_EOF) {
    const char *item = parser->current_token.value;
    if (!item) {
      parser_set_error(parser, "Expected barrier memory region or order");
      return NULL;
    }
    unsigned region = 0;
    AstMemoryOrder parsed_order = AST_MEMORY_ORDER_SEQ_CST;
    int is_order = 0;
    if (strcmp(item, "workgroup") == 0) {
      region = AST_MEMORY_REGION_WORKGROUP;
    } else if (strcmp(item, "global") == 0) {
      region = AST_MEMORY_REGION_GLOBAL;
    } else if (strcmp(item, "acquire") == 0) {
      parsed_order = AST_MEMORY_ORDER_ACQUIRE;
      is_order = 1;
    } else if (strcmp(item, "release") == 0) {
      parsed_order = AST_MEMORY_ORDER_RELEASE;
      is_order = 1;
    } else if (strcmp(item, "acq_rel") == 0) {
      parsed_order = AST_MEMORY_ORDER_ACQ_REL;
      is_order = 1;
    } else if (strcmp(item, "seq_cst") == 0) {
      parsed_order = AST_MEMORY_ORDER_SEQ_CST;
      is_order = 1;
    } else {
      parser_set_error(parser,
                       "Barrier arguments are workgroup/global memory regions "
                       "or acquire/release/acq_rel/seq_cst orders");
      return NULL;
    }
    if (is_order) {
      if (saw_order) {
        parser_set_error(parser, "Barrier accepts exactly one memory order");
        return NULL;
      }
      saw_order = 1;
      order = parsed_order;
    } else {
      if (regions & region) {
        parser_set_error(parser, "Duplicate barrier memory region");
        return NULL;
      }
      regions |= region;
    }
    parser_advance(parser);
    if (parser->current_token.type == TOKEN_COMMA) {
      parser_advance(parser);
      if (parser->current_token.type == TOKEN_RPAREN) {
        parser_set_error(parser, "Trailing comma in barrier contract");
        return NULL;
      }
    } else if (parser->current_token.type != TOKEN_RPAREN) {
      parser_set_error(parser, "Expected ',' or ')' in barrier contract");
      return NULL;
    }
  }
  if (!parser_expect(parser, TOKEN_RPAREN)) return NULL;
  if (regions == 0) regions = AST_MEMORY_REGION_WORKGROUP;
  parser_expect_statement_end(parser);
  return ast_create_barrier_statement(regions, order, location);
}

static int parser_parse_parameter_list(Parser *parser, char ***out_names,
                                       char ***out_types,
                                       size_t *out_count) {
  char **param_names = NULL;
  char **param_types = NULL;
  size_t param_count = 0;
  parser->group_context = "parameter list";

  if (parser->current_token.type != TOKEN_RPAREN) {
    do {
      if (!parser_is_identifier_like(parser->current_token.type)) {
        if (parser->current_token.type == TOKEN_RPAREN && param_count > 0)
          parser_set_error_with_suggestion(
              parser, "Expected a parameter name, found ')'",
              "the parameter list ends with a ','; remove it");
        else
          parser_set_error(parser, "Expected a parameter name");
        goto fail;
      }

      param_names = realloc(param_names, (param_count + 1) * sizeof(char *));
      param_types = realloc(param_types, (param_count + 1) * sizeof(char *));

      param_names[param_count] = strdup(parser->current_token.value);
      parser_advance(parser);

      if (!parser_expect(parser, TOKEN_COLON)) {
        free(param_names[param_count]);
        goto fail;
      }

      param_types[param_count] = parser_parse_type_annotation(parser);
      if (!param_types[param_count]) {
        if (!parser->has_error) {
          parser_set_error(parser, "Expected parameter type");
        }
        free(param_names[param_count]);
        goto fail;
      }
      param_count++;

      if (parser->current_token.type == TOKEN_COMMA) {
        parser_advance(parser);
      } else if (parser->current_token.type == TOKEN_RPAREN) {
        break;
      } else {
        parser_set_error(parser, "Expected ',' or ')' in parameter list");
        goto fail;
      }
    } while (1);
  }

  *out_names = param_names;
  *out_types = param_types;
  *out_count = param_count;
  parser->group_context = NULL;
  return 1;

fail:
  for (size_t i = 0; i < param_count; i++) {
    free(param_names[i]);
    free(param_types[i]);
  }
  free(param_names);
  free(param_types);
  parser->group_context = NULL;
  return 0;
}

ASTNode *parser_parse_function_declaration(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  if (parser->current_token.type != TOKEN_FUNCTION &&
      parser->current_token.type != TOKEN_FN &&
      parser->current_token.type != TOKEN_KERNEL) {
    parser_set_error(parser, "Expected 'fn' or 'kernel'");
    return NULL;
  }
  /* Preserve kernel identity. The declaration otherwise shares the function
   * grammar, but semantic analysis and the backend treat it as a GPU entry
   * point rather than an ordinary host/device helper function. */
  int is_kernel = parser->current_token.type == TOKEN_KERNEL;
  parser_advance(parser);

  /* `kernel(block = 256)` / `kernel(block = (x, y, z))`: the launch block
   * shape this kernel requires. Recorded on the declaration so the backends
   * can stamp it into the module (.reqntid / LocalSize) and the launch is
   * rejected by the driver instead of running with a garbage lane mapping. */
  int kernel_block[3] = {0, 0, 0};
  int kernel_threads_per_item = 0;
  if (is_kernel && parser->current_token.type == TOKEN_LPAREN) {
    parser_advance(parser);
    if (parser->current_token.type != TOKEN_IDENTIFIER ||
        strcmp(parser->current_token.value, "block") != 0) {
      parser_set_error(parser,
                       "Expected 'block' in kernel attribute list "
                       "(kernel(block = N) or kernel(block = (x, y, z)))");
      return NULL;
    }
    parser_advance(parser);
    if (!parser_expect(parser, TOKEN_EQUALS)) {
      return NULL;
    }
    int dims = 1;
    int grouped = parser->current_token.type == TOKEN_LPAREN;
    if (grouped) {
      parser_advance(parser);
      dims = 3;
    }
    long long product = 1;
    for (int d = 0; d < dims; d++) {
      if (d && !parser_expect(parser, TOKEN_COMMA)) {
        return NULL;
      }
      if (parser->current_token.type != TOKEN_NUMBER ||
          strchr(parser->current_token.value, '.')) {
        parser_set_error(parser,
                         "Kernel block dimensions must be positive integer "
                         "literals");
        return NULL;
      }
      long long value = strtoll(parser->current_token.value, NULL, 0);
      if (value < 1 || value > 1024) {
        parser_set_error(parser,
                         "Kernel block dimension must be between 1 and 1024");
        return NULL;
      }
      kernel_block[d] = (int)value;
      product *= value;
      parser_advance(parser);
    }
    if (grouped && !parser_expect(parser, TOKEN_RPAREN)) {
      return NULL;
    }
    if (dims == 1) {
      kernel_block[1] = 1;
      kernel_block[2] = 1;
    }
    if (product > 1024) {
      parser_set_error(parser,
                       "Kernel block volume must not exceed 1024 work-items");
      return NULL;
    }
    /* `per = thread` (the default) or `per = warp`: how many threads one unit
     * of work costs. A warp-per-row matvec covers block_volume/32 rows per
     * block, not block_volume, and `dispatch k[work: n]` has to know which. */
    if (parser->current_token.type == TOKEN_COMMA) {
      parser_advance(parser);
      if (parser->current_token.type != TOKEN_IDENTIFIER ||
          strcmp(parser->current_token.value, "per") != 0) {
        parser_set_error(parser,
                         "Expected 'per' after the kernel block shape "
                         "(kernel(block = N, per = warp))");
        return NULL;
      }
      parser_advance(parser);
      if (!parser_expect(parser, TOKEN_EQUALS)) {
        return NULL;
      }
      if (parser->current_token.type != TOKEN_IDENTIFIER) {
        parser_set_error(parser, "Expected 'thread' or 'warp' after 'per ='");
        return NULL;
      }
      if (strcmp(parser->current_token.value, "warp") == 0) {
        kernel_threads_per_item = 32;
      } else if (strcmp(parser->current_token.value, "thread") == 0) {
        kernel_threads_per_item = 1;
      } else {
        parser_set_error(parser, "Kernel 'per' must be 'thread' or 'warp'");
        return NULL;
      }
      if (product % (kernel_threads_per_item > 0 ? kernel_threads_per_item : 1)
          != 0) {
        parser_set_error(parser,
                         "A 'per = warp' kernel needs a block volume that is a "
                         "whole number of 32-lane warps");
        return NULL;
      }
      parser_advance(parser);
    }
    if (!parser_expect(parser, TOKEN_RPAREN)) {
      return NULL;
    }
  }

  // Expect function name
  char *func_name = NULL;
  ASTNode *composed_name = NULL;
  if (!parser_parse_declaration_name(parser, "Expected function name after 'fn'",
                                     &func_name, &composed_name)) {
    return NULL;
  }

  char **func_type_params = NULL;
  char **func_type_param_traits = NULL;
  size_t func_type_param_count = 0;
  if (parser->current_token.type == TOKEN_LESS_THAN) {
    func_type_params = parser_parse_type_param_list(
        parser, &func_type_param_traits, &func_type_param_count);
    if (!func_type_params && parser->has_error) {
      free(func_name);
      return NULL;
    }
  }

  // Expect '('
  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_free_type_param_list(func_type_params, func_type_param_traits,
                                func_type_param_count);
    free(func_name);
    return NULL;
  }

  char **param_names = NULL;
  char **param_types = NULL;
  size_t param_count = 0;

  if (!parser_parse_parameter_list(parser, &param_names, &param_types,
                                   &param_count)) {
    parser_free_type_param_list(func_type_params, func_type_param_traits,
                                func_type_param_count);
    free(func_name);
    return NULL;
  }

  // Expect ')'
  if (!parser_expect(parser, TOKEN_RPAREN)) {
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    parser_free_type_param_list(func_type_params, func_type_param_traits,
                                func_type_param_count);
    free(func_name);
    return NULL;
  }

  // Optional return type: '-> type' (or ': type' for compatibility)
  char *return_type = NULL;
  char **return_types = NULL;
  size_t return_type_count = 0;
  char *link_name = NULL;
  if (parser->current_token.type == TOKEN_ARROW ||
      parser->current_token.type == TOKEN_COLON) {
    parser_advance(parser); // consume return separator

    if (parser->current_token.type == TOKEN_LPAREN) {
      return_types = parser_parse_multi_return_types(parser, &return_type_count);
      return_type = return_types
                        ? parser_make_multi_return_name(func_name)
                        : NULL;
    } else {
      return_type = parser_parse_type_annotation(parser);
    }
    if (!return_type) {
      if (!parser->has_error) {
        parser_set_error(parser, "Expected return type after return separator");
      }
      // Clean up
      for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
        free(param_types[i]);
      }
      free(param_names);
      free(param_types);
      parser_free_type_param_list(func_type_params, func_type_param_traits,
                                  func_type_param_count);
      free(func_name);
      free(link_name);
      parser_free_string_array(return_types, return_type_count);
      return NULL;
    }
  }

  if (parser->current_token.type == TOKEN_WHERE &&
      !parser_parse_where_clause(parser, func_type_params,
                                 func_type_param_traits,
                                 func_type_param_count)) {
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    parser_free_type_param_list(func_type_params, func_type_param_traits,
                                func_type_param_count);
    free(func_name);
    free(return_type);
    free(link_name);
    parser_free_string_array(return_types, return_type_count);
    return NULL;
  }

  ParsedEffectClauses effect_clauses;
  if (!parser_parse_effect_clauses(parser, &effect_clauses)) {
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    parser_free_type_param_list(func_type_params, func_type_param_traits,
                                func_type_param_count);
    free(func_name);
    free(return_type);
    free(link_name);
    parser_free_string_array(return_types, return_type_count);
    return NULL;
  }

  char *reference_twin = NULL;
  char *explain_code = NULL;
  char *explain_text = NULL;
  if (parser_is_identifier_like(parser->current_token.type) &&
      parser->current_token.value &&
      strcmp(parser->current_token.value, "reference") == 0) {
    parser_advance(parser);
    if (!parser_is_identifier_like(parser->current_token.type) ||
        !parser->current_token.value) {
      parser_set_error(parser,
                       "Expected the name of the reference function after "
                       "'reference'");
    } else {
      reference_twin = strdup(parser->current_token.value);
      parser_advance(parser);
    }
  }
  if (parser_is_identifier_like(parser->current_token.type) &&
      parser->current_token.value &&
      strcmp(parser->current_token.value, "explain") == 0) {
    parser_advance(parser);
    if (!parser_is_identifier_like(parser->current_token.type) ||
        !parser->current_token.value ||
        parser->current_token.value[0] != 'R') {
      parser_set_error(parser,
                       "Expected a diagnostic code such as R1001 after "
                       "'explain'");
    } else {
      explain_code = strdup(parser->current_token.value);
      parser_advance(parser);
      if (parser->current_token.type != TOKEN_STRING) {
        parser_set_error(parser,
                         "Expected the explanation text after the code");
      } else {
        explain_text = strdup(parser->current_token.value);
        parser_advance(parser);
      }
    }
  }

  if (parser->current_token.type == TOKEN_EQUALS) {
    parser_advance(parser); // consume '='
    if (parser->current_token.type != TOKEN_STRING) {
      parser_set_error(parser, "Expected string literal link name after '='");
      for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
        free(param_types[i]);
      }
      free(param_names);
      free(param_types);
      parser_free_type_param_list(func_type_params, func_type_param_traits,
                                  func_type_param_count);
      free(func_name);
      free(return_type);
      parser_free_string_array(return_types, return_type_count);
      parser_free_effect_clauses(&effect_clauses);
      return NULL;
    }
    link_name = strdup(parser->current_token.value);
    parser_advance(parser);
    if (!link_name) {
      parser_set_error(parser, "Memory allocation failed for link name");
      for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
        free(param_types[i]);
      }
      free(param_names);
      free(param_types);
      free(func_name);
      free(return_type);
      parser_free_string_array(return_types, return_type_count);
      parser_free_effect_clauses(&effect_clauses);
      return NULL;
    }
  }

  // Parse function body (block) or allow forward declaration terminator
  ASTNode *body = NULL;
  if (parser->current_token.type == TOKEN_LBRACE) {
    body = parser_parse_block(parser);
    if (!body && parser->has_error) {
      // Clean up
      for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
        free(param_types[i]);
      }
      free(param_names);
      free(param_types);
      parser_free_type_param_list(func_type_params, func_type_param_traits,
                                  func_type_param_count);
      free(func_name);
      free(return_type);
      free(link_name);
      parser_free_string_array(return_types, return_type_count);
      parser_free_effect_clauses(&effect_clauses);
      return NULL;
    }
  } else if (parser->current_token.type == TOKEN_SEMICOLON ||
             parser->current_token.type == TOKEN_NEWLINE) {
    parser_expect_statement_end(parser);
  } else {
    parser_set_error(parser,
                     "Expected function body ('{') or declaration terminator");
    // Clean up
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    parser_free_type_param_list(func_type_params, func_type_param_traits,
                                func_type_param_count);
    free(func_name);
    free(return_type);
    free(link_name);
    parser_free_string_array(return_types, return_type_count);
    parser_free_effect_clauses(&effect_clauses);
    return NULL;
  }

  ASTNode *func_decl =
      ast_create_function_declaration(func_name, param_names, param_types,
                                      param_count, return_type, body, location);
  if (func_decl && func_decl->data &&
      !parser_apply_effect_clauses((FunctionDeclaration *)func_decl->data,
                                   &effect_clauses)) {
    parser_set_error(parser, "Memory allocation failed for effect clauses");
  }
  parser_free_effect_clauses(&effect_clauses);
  if (func_decl && func_decl->data) {
    FunctionDeclaration *func_data = (FunctionDeclaration *)func_decl->data;
    func_data->reference_twin = reference_twin;
    reference_twin = NULL;
    func_data->explain_code = explain_code;
    func_data->explain_text = explain_text;
    explain_code = NULL;
    explain_text = NULL;
    func_data->composed_name = composed_name;
    composed_name = NULL;
    parser->pending_composed_name = NULL;
    func_data->return_types = return_types;
    func_data->return_type_count = return_type_count;
    return_types = NULL;
    return_type_count = 0;
    func_data->is_kernel = is_kernel;
    func_data->kernel_block[0] = kernel_block[0];
    func_data->kernel_block[1] = kernel_block[1];
    func_data->kernel_block[2] = kernel_block[2];
    func_data->kernel_threads_per_item = kernel_threads_per_item;
    if (link_name) {
      func_data->link_name = strdup(link_name);
      if (!func_data->link_name) {
        ast_destroy_node(func_decl);
        func_decl = NULL;
        parser_set_error(parser, "Memory allocation failed for link name");
      }
    }
  }
  if (func_decl && func_decl->data && func_type_param_count > 0) {
    FunctionDeclaration *fd = (FunctionDeclaration *)func_decl->data;
    fd->type_params = malloc(func_type_param_count * sizeof(char *));
    fd->type_param_traits = malloc(func_type_param_count * sizeof(char *));
    fd->type_param_count = func_type_param_count;
    for (size_t i = 0; i < func_type_param_count; i++) {
      fd->type_params[i] = (char *)string_intern(func_type_params[i]);
      fd->type_param_traits[i] = func_type_param_traits[i]
                                     ? (char *)string_intern(
                                           func_type_param_traits[i])
                                     : NULL;
    }
  }
  parser_free_type_param_list(func_type_params, func_type_param_traits,
                              func_type_param_count);

  // Clean up temporary strings
  free(func_name);
  free(return_type);
  free(link_name);
  free(reference_twin);
  free(explain_code);
  free(explain_text);
  parser_free_string_array(return_types, return_type_count);
  for (size_t i = 0; i < param_count; i++) {
    free(param_names[i]);
    free(param_types[i]);
  }
  free(param_names);
  free(param_types);

  return func_decl;
}

ASTNode *parser_parse_trait_declaration(Parser *parser) {
  if (!parser) {
    return NULL;
  }

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, TOKEN_TRAIT)) {
    return NULL;
  }

  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected trait name after 'trait'");
    return NULL;
  }

  char *trait_name = strdup(parser->current_token.value);
  ASTNode *trait_decl = NULL;
  ASTNode **methods = NULL;
  size_t method_count = 0;
  parser_advance(parser);

  if (!trait_name) {
    return NULL;
  }

  if (parser->current_token.type == TOKEN_LBRACE) {
    parser_advance(parser);
    while (parser->current_token.type != TOKEN_RBRACE &&
           parser->current_token.type != TOKEN_EOF && !parser->has_error) {
      if (parser->current_token.type == TOKEN_NEWLINE ||
          parser->current_token.type == TOKEN_SEMICOLON) {
        parser_advance(parser);
        continue;
      }
      if (parser->current_token.type != TOKEN_FUNCTION &&
          parser->current_token.type != TOKEN_FN) {
        parser_set_error(parser,
                         "Expected function signature in trait declaration");
        goto trait_cleanup;
      }
      ASTNode *method = parser_parse_function_declaration(parser);
      if (!method) {
        goto trait_cleanup;
      }
      FunctionDeclaration *method_decl = (FunctionDeclaration *)method->data;
      if (method_decl && method_decl->body) {
        parser_set_error(parser, "Trait method declarations must not have a body");
        ast_destroy_node(method);
        goto trait_cleanup;
      }
      ASTNode **grown = realloc(methods, (method_count + 1) * sizeof(ASTNode *));
      if (!grown) {
        parser_set_error(parser, "Out of memory while parsing trait methods");
        ast_destroy_node(method);
        goto trait_cleanup;
      }
      methods = grown;
      methods[method_count++] = method;
    }
    if (!parser_expect(parser, TOKEN_RBRACE)) {
      goto trait_cleanup;
    }
  } else if (!parser_expect_statement_end(parser)) {
    goto trait_cleanup;
  }

  trait_decl = ast_create_trait_declaration(trait_name, location);
  if (trait_decl && trait_decl->data && method_count > 0) {
    TraitDeclaration *data = (TraitDeclaration *)trait_decl->data;
    data->methods = malloc(method_count * sizeof(ASTNode *));
    if (!data->methods) {
      parser_set_error(parser, "Out of memory while parsing trait methods");
      ast_destroy_node(trait_decl);
      trait_decl = NULL;
      goto trait_cleanup;
    }
    data->method_count = method_count;
    for (size_t i = 0; i < method_count; i++) {
      data->methods[i] = methods[i];
      ast_add_child(trait_decl, methods[i]);
      methods[i] = NULL;
    }
  }

trait_cleanup:
  for (size_t i = 0; i < method_count; i++) {
    if (methods[i]) {
      ast_destroy_node(methods[i]);
    }
  }
  free(methods);
  free(trait_name);
  return trait_decl;
}

ASTNode *parser_parse_impl_declaration(Parser *parser) {
  if (!parser) {
    return NULL;
  }

  SourceLocation location = parser_current_location(parser);
  char *trait_name = NULL;
  char *for_type_name = NULL;
  ASTNode *impl_decl = NULL;
  ASTNode **methods = NULL;
  size_t method_count = 0;

  if (!parser_expect(parser, TOKEN_IMPL)) {
    return NULL;
  }

  trait_name =
      parser_parse_qualified_name(parser, "Expected trait name after 'impl'");
  if (!trait_name) {
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_FOR)) {
    free(trait_name);
    parser_set_error(parser, "Expected 'for' after trait name in impl");
    return NULL;
  }

  for_type_name = parser_parse_type_annotation(parser);
  if (!for_type_name) {
    if (!parser->has_error) {
      parser_set_error(parser, "Expected type name after 'for' in impl");
    }
    free(trait_name);
    return NULL;
  }

  if (parser->current_token.type == TOKEN_LBRACE) {
    parser_advance(parser);
    while (parser->current_token.type != TOKEN_RBRACE &&
           parser->current_token.type != TOKEN_EOF && !parser->has_error) {
      if (parser->current_token.type == TOKEN_NEWLINE ||
          parser->current_token.type == TOKEN_SEMICOLON) {
        parser_advance(parser);
        continue;
      }
      if (parser->current_token.type != TOKEN_FUNCTION &&
          parser->current_token.type != TOKEN_FN) {
        parser_set_error(parser, "Expected function declaration in impl block");
        goto impl_cleanup;
      }
      ASTNode *method = parser_parse_function_declaration(parser);
      if (!method) {
        goto impl_cleanup;
      }
      FunctionDeclaration *method_decl = (FunctionDeclaration *)method->data;
      if (method_decl && !method_decl->body) {
        parser_set_error(parser, "Impl method declarations must have a body");
        ast_destroy_node(method);
        goto impl_cleanup;
      }
      ASTNode **grown = realloc(methods, (method_count + 1) * sizeof(ASTNode *));
      if (!grown) {
        parser_set_error(parser, "Out of memory while parsing impl methods");
        ast_destroy_node(method);
        goto impl_cleanup;
      }
      methods = grown;
      methods[method_count++] = method;
    }
    if (!parser_expect(parser, TOKEN_RBRACE)) {
      goto impl_cleanup;
    }
  } else if (!parser_expect_statement_end(parser)) {
    goto impl_cleanup;
  }

  impl_decl = ast_create_impl_declaration(trait_name, for_type_name, location);
  if (impl_decl && impl_decl->data && method_count > 0) {
    ImplDeclaration *data = (ImplDeclaration *)impl_decl->data;
    data->methods = malloc(method_count * sizeof(ASTNode *));
    if (!data->methods) {
      parser_set_error(parser, "Out of memory while parsing impl methods");
      ast_destroy_node(impl_decl);
      impl_decl = NULL;
      goto impl_cleanup;
    }
    data->method_count = method_count;
    for (size_t i = 0; i < method_count; i++) {
      data->methods[i] = methods[i];
      ast_add_child(impl_decl, methods[i]);
      methods[i] = NULL;
    }
  }

impl_cleanup:
  for (size_t i = 0; i < method_count; i++) {
    if (methods[i]) {
      ast_destroy_node(methods[i]);
    }
  }
  free(methods);
  free(trait_name);
  free(for_type_name);
  return impl_decl;
}

ASTNode *parser_parse_enum_declaration(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, TOKEN_ENUM))
    return NULL;

  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected enum name after 'enum'");
    return NULL;
  }
  char *enum_name = strdup(parser->current_token.value);
  parser_advance(parser);

  // Optional generic type parameters: enum Option<T> { ... }
  char **type_params = NULL;
  size_t type_param_count = 0;
  if (parser->current_token.type == TOKEN_LESS_THAN) {
    parser_advance(parser); // consume '<'
    while (parser->current_token.type != TOKEN_GREATER_THAN &&
           parser->current_token.type != TOKEN_EOF && !parser->has_error) {
      if (parser->current_token.type == TOKEN_COMMA ||
          parser->current_token.type == TOKEN_NEWLINE) {
        parser_advance(parser);
        continue;
      }
      if (!parser_is_identifier_like(parser->current_token.type) &&
          !parser_is_type_keyword(parser->current_token.type)) {
        parser_set_error(parser, "Expected type parameter name");
        break;
      }
      char **new_tp =
          realloc(type_params, (type_param_count + 1) * sizeof(char *));
      if (!new_tp) {
        parser_set_error(parser, "Out of memory in enum type parameters");
        break;
      }
      type_params = new_tp;
      type_params[type_param_count++] = strdup(parser->current_token.value);
      parser_advance(parser);
    }
    if (!parser->has_error && !parser_expect(parser, TOKEN_GREATER_THAN)) {
      for (size_t i = 0; i < type_param_count; i++) free(type_params[i]);
      free(type_params);
      free(enum_name);
      return NULL;
    }
  }

  if (!parser_expect(parser, TOKEN_LBRACE)) {
    for (size_t i = 0; i < type_param_count; i++) free(type_params[i]);
    free(type_params);
    free(enum_name);
    return NULL;
  }

  EnumVariant *variants = NULL;
  size_t variant_count = 0;

  while (parser->current_token.type != TOKEN_RBRACE &&
         parser->current_token.type != TOKEN_EOF && !parser->has_error) {
    if (parser->current_token.type == TOKEN_NEWLINE ||
        parser->current_token.type == TOKEN_COMMA) {
      parser_advance(parser);
      continue;
    }

    if (!parser_is_identifier_like(parser->current_token.type)) {
      parser_set_error(parser, "Expected enum variant name");
      break;
    }
    char *variant_name = strdup(parser->current_token.value);
    parser_advance(parser);

    // Optional payload type: Some(T)
    char *payload_type = NULL;
    if (parser->current_token.type == TOKEN_LPAREN) {
      parser_advance(parser); // consume '('
      if (!parser_is_identifier_like(parser->current_token.type) &&
          !parser_is_type_keyword(parser->current_token.type)) {
        parser_set_error(parser, "Expected payload type in variant");
        free(variant_name);
        break;
      }
      payload_type = parser_parse_type_annotation(parser);
      if (!payload_type) {
        parser_set_error(parser, "Expected payload type in variant");
        free(variant_name);
        break;
      }
      if (!parser_expect(parser, TOKEN_RPAREN)) {
        free(payload_type);
        free(variant_name);
        break;
      }
    }

    ASTNode *value = NULL;
    if (parser->current_token.type == TOKEN_EQUALS) {
      parser_advance(parser);
      value = parser_parse_expression(parser);
      if (!value) {
        if (!parser->has_error)
          parser_set_error(parser, "Expected expression after '='");
        free(payload_type);
        free(variant_name);
        break;
      }
    }

    EnumVariant *new_v =
        realloc(variants, (variant_count + 1) * sizeof(EnumVariant));
    if (!new_v) {
      parser_set_error(parser, "Out of memory");
      free(payload_type);
      free(variant_name);
      break;
    }
    variants = new_v;
    variants[variant_count].name = variant_name;
    variants[variant_count].payload_type = payload_type;
    variants[variant_count].value = value;
    variant_count++;

    if (parser->current_token.type == TOKEN_COMMA ||
        parser->current_token.type == TOKEN_NEWLINE) {
      parser_advance(parser);
    }
  }

  if (parser->has_error) {
    for (size_t i = 0; i < variant_count; i++) {
      free(variants[i].name);
      free(variants[i].payload_type);
      ast_destroy_node(variants[i].value);
    }
    free(variants);
    for (size_t i = 0; i < type_param_count; i++) free(type_params[i]);
    free(type_params);
    free(enum_name);
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_RBRACE)) {
    for (size_t i = 0; i < variant_count; i++) {
      free(variants[i].name);
      free(variants[i].payload_type);
      ast_destroy_node(variants[i].value);
    }
    free(variants);
    for (size_t i = 0; i < type_param_count; i++) free(type_params[i]);
    free(type_params);
    free(enum_name);
    return NULL;
  }

  ASTNode *node =
      ast_create_enum_declaration(enum_name, variants, variant_count, location);

  if (node) {
    EnumDeclaration *decl = (EnumDeclaration *)node->data;
    if (decl && type_param_count > 0) {
      decl->type_params = type_params;
      decl->type_param_count = type_param_count;
      type_params = NULL; // ownership transferred
      type_param_count = 0;
    }
  }

  free(enum_name);
  for (size_t i = 0; i < variant_count; i++) {
    free(variants[i].name);
    free(variants[i].payload_type);
  }
  free(variants);
  for (size_t i = 0; i < type_param_count; i++) free(type_params[i]);
  free(type_params);

  return node;
}

ASTNode *parser_parse_struct_declaration(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  // Expect 'struct' keyword
  if (!parser_expect(parser, TOKEN_STRUCT)) {
    return NULL;
  }

  // Expect struct name
  char *struct_name = NULL;
  ASTNode *composed_name = NULL;
  if (!parser_parse_declaration_name(
          parser, "Expected struct name after 'struct'", &struct_name,
          &composed_name)) {
    return NULL;
  }

  char **type_params = NULL;
  char **type_param_traits = NULL;
  size_t type_param_count = 0;
  if (parser->current_token.type == TOKEN_LESS_THAN) {
    type_params = parser_parse_type_param_list(parser, &type_param_traits,
                                               &type_param_count);
    if (!type_params && parser->has_error) {
      free(struct_name);
      return NULL;
    }
  }

  if (parser->current_token.type == TOKEN_WHERE &&
      !parser_parse_where_clause(parser, type_params, type_param_traits,
                                 type_param_count)) {
    parser_free_type_param_list(type_params, type_param_traits,
                                type_param_count);
    free(struct_name);
    return NULL;
  }

  // Expect '{'
  if (!parser_expect(parser, TOKEN_LBRACE)) {
    parser_free_type_param_list(type_params, type_param_traits,
                                type_param_count);
    free(struct_name);
    return NULL;
  }

  // Parse fields and methods
  char **field_names = NULL;
  char **field_types = NULL;
  size_t field_count = 0;
  ASTNode **methods = NULL;
  size_t method_count = 0;

  while (parser->current_token.type != TOKEN_RBRACE &&
         parser->current_token.type != TOKEN_EOF && !parser->has_error) {

    // Allow blank lines and redundant separators inside struct bodies.
    if (parser->current_token.type == TOKEN_NEWLINE ||
        parser->current_token.type == TOKEN_SEMICOLON) {
      parser_advance(parser);
      continue;
    }

    if (parser->current_token.type == TOKEN_METHOD) {
      // Parse method declaration
      ASTNode *method = parser_parse_method_declaration(parser);
      if (method) {
        methods = realloc(methods, (method_count + 1) * sizeof(ASTNode *));
        methods[method_count] = method;
        method_count++;
      } else if (parser->has_error) {
        // Clean up and return
        for (size_t i = 0; i < field_count; i++) {
          free(field_names[i]);
          free(field_types[i]);
        }
        free(field_names);
        free(field_types);
        for (size_t i = 0; i < method_count; i++) {
          ast_destroy_node(methods[i]);
        }
        free(methods);
        parser_free_type_param_list(type_params, type_param_traits,
                                    type_param_count);
        free(struct_name);
        return NULL;
      }
    } else {
      // Parse field declaration: name: type;
      if (!parser_is_identifier_like(parser->current_token.type)) {
        parser_set_error(parser, "Expected field name or method declaration");
        // Clean up
        for (size_t i = 0; i < field_count; i++) {
          free(field_names[i]);
          free(field_types[i]);
        }
        free(field_names);
        free(field_types);
        for (size_t i = 0; i < method_count; i++) {
          ast_destroy_node(methods[i]);
        }
        free(methods);
        parser_free_type_param_list(type_params, type_param_traits,
                                    type_param_count);
        free(struct_name);
        return NULL;
      }

      // Reallocate field arrays
      field_names = realloc(field_names, (field_count + 1) * sizeof(char *));
      field_types = realloc(field_types, (field_count + 1) * sizeof(char *));

      field_names[field_count] = strdup(parser->current_token.value);
      parser_advance(parser);

      // Expect ':'
      if (!parser_expect(parser, TOKEN_COLON)) {
        // Clean up
        for (size_t i = 0; i <= field_count; i++) {
          free(field_names[i]);
          if (i < field_count)
            free(field_types[i]);
        }
        free(field_names);
        free(field_types);
        for (size_t i = 0; i < method_count; i++) {
          ast_destroy_node(methods[i]);
        }
        free(methods);
        parser_free_type_param_list(type_params, type_param_traits,
                                    type_param_count);
        free(struct_name);
        return NULL;
      }

      // Parse field type
      field_types[field_count] = parser_parse_type_annotation(parser);
      if (!field_types[field_count]) {
        if (!parser->has_error) {
          parser_set_error(parser, "Expected field type");
        }
        // Clean up
        for (size_t i = 0; i <= field_count; i++) {
          free(field_names[i]);
          if (i < field_count)
            free(field_types[i]);
        }
        free(field_names);
        free(field_types);
        for (size_t i = 0; i < method_count; i++) {
          ast_destroy_node(methods[i]);
        }
        free(methods);
        parser_free_type_param_list(type_params, type_param_traits,
                                    type_param_count);
        free(struct_name);
        return NULL;
      }
      field_count++;

      // Expect a newline or semicolon to end the declaration
      parser_expect_statement_end(parser);
    }
  }

  // Expect '}'
  if (!parser_expect(parser, TOKEN_RBRACE)) {
    // Clean up
    for (size_t i = 0; i < field_count; i++) {
      free(field_names[i]);
      free(field_types[i]);
    }
    free(field_names);
    free(field_types);
    for (size_t i = 0; i < method_count; i++) {
      ast_destroy_node(methods[i]);
    }
    free(methods);
    parser_free_type_param_list(type_params, type_param_traits,
                                type_param_count);
    free(struct_name);
    return NULL;
  }

  ASTNode *struct_decl = ast_create_struct_declaration(
      struct_name, field_names, field_types, field_count, methods, method_count,
      location);

  if (struct_decl && struct_decl->data) {
    ((StructDeclaration *)struct_decl->data)->composed_name = composed_name;
    composed_name = NULL;
    parser->pending_composed_name = NULL;
  }
  ast_destroy_node(composed_name);

  if (struct_decl && struct_decl->data && type_param_count > 0) {
    StructDeclaration *sd = (StructDeclaration *)struct_decl->data;
    sd->type_params = malloc(type_param_count * sizeof(char *));
    sd->type_param_traits = malloc(type_param_count * sizeof(char *));
    sd->type_param_count = type_param_count;
    for (size_t i = 0; i < type_param_count; i++) {
      sd->type_params[i] = (char *)string_intern(type_params[i]);
      sd->type_param_traits[i] =
          type_param_traits[i] ? (char *)string_intern(type_param_traits[i])
                               : NULL;
    }
  }
  parser_free_type_param_list(type_params, type_param_traits, type_param_count);

  free(struct_name);
  for (size_t i = 0; i < field_count; i++) {
    free(field_names[i]);
    free(field_types[i]);
  }
  free(field_names);
  free(field_types);
  // Note: methods array is now owned by the AST node, don't free it

  return struct_decl;
}

ASTNode *parser_parse_inline_asm(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, TOKEN_ASM)) {
    return NULL;
  }

  if (!parser_match(parser, TOKEN_LBRACE)) {
    parser_set_error(parser, "expected '{' to open an asm block");
    return NULL;
  }

  const char *source = parser->lexer->source;
  if (!source || !parser->current_token.lexeme.data) {
    parser_set_error(parser, "asm block source text is unavailable");
    return NULL;
  }

  size_t open_offset = (size_t)(parser->current_token.lexeme.data - source);
  size_t scan = open_offset + 1;
  size_t start_line = parser->current_token.line;
  int depth = 1;
  size_t line = start_line;

  while (source[scan] && depth > 0) {
    char c = source[scan];
    if (c == '\n') {
      line++;
      scan++;
      continue;
    }
    if (c == ';' || (c == '/' && source[scan + 1] == '/')) {
      while (source[scan] && source[scan] != '\n') {
        scan++;
      }
      continue;
    }
    if (c == '/' && source[scan + 1] == '*') {
      scan += 2;
      while (source[scan] && !(source[scan] == '*' && source[scan + 1] == '/')) {
        if (source[scan] == '\n') {
          line++;
        }
        scan++;
      }
      if (source[scan]) {
        scan += 2;
      }
      continue;
    }
    if (c == '\'' || c == '"') {
      char quote = c;
      scan++;
      while (source[scan] && source[scan] != quote) {
        if (source[scan] == '\\' && source[scan + 1]) {
          scan++;
        } else if (source[scan] == '\n') {
          line++;
        }
        scan++;
      }
      if (source[scan]) {
        scan++;
      }
      continue;
    }
    if (c == '{') {
      depth++;
    } else if (c == '}') {
      depth--;
      if (depth == 0) {
        break;
      }
    }
    scan++;
  }

  if (depth != 0) {
    parser_set_error(parser, "unterminated asm block: expected '}'");
    return NULL;
  }

  size_t body_length = scan - (open_offset + 1);
  char *assembly_code = (char *)malloc(body_length + 1);
  if (!assembly_code) {
    parser_set_error(parser, "Memory allocation failed");
    return NULL;
  }
  memcpy(assembly_code, source + open_offset + 1, body_length);
  assembly_code[body_length] = '\0';

  token_destroy(&parser->current_token);
  token_destroy(&parser->peek_token);
  memset(&parser->current_token, 0, sizeof(parser->current_token));
  memset(&parser->peek_token, 0, sizeof(parser->peek_token));

  parser->lexer->position = scan + 1;
  parser->lexer->line = line;
  parser->lexer->column = 1;
  parser->lexer->continuation_depth = 0;
  parser->lexer->last_significant = TOKEN_RBRACE;
  parser->current_token = lexer_next_token(parser->lexer);
  parser->peek_token = lexer_next_token(parser->lexer);
  parser->previous_token_type = TOKEN_RBRACE;
  snprintf(parser->previous_token_text, sizeof(parser->previous_token_text),
           "}");

  ASTNode *inline_asm = ast_create_inline_asm(assembly_code, location);
  free(assembly_code);
  return inline_asm;
}

ASTNode *parser_parse_return_statement(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  parser_advance(parser); // consume 'return'

  ASTNode *value = NULL;
  ASTNode **values = NULL;
  size_t value_count = 0;
  if (parser->current_token.type != TOKEN_SEMICOLON &&
      parser->current_token.type != TOKEN_NEWLINE) {
    if (parser->current_token.type == TOKEN_LPAREN) {
      ParserSavedState saved = parser_save_state(parser);
      parser->error_message = NULL;
      parser->error_reporter = NULL;
      value = parser_parse_expression(parser);
      if (value && parser->current_token.type != TOKEN_COMMA) {
        parser->error_reporter = saved.error_reporter;
        parser_discard_saved_state(&saved);
      } else {
        if (value) {
          ast_destroy_node(value);
          value = NULL;
        }
        parser_restore_state(parser, &saved);
        parser_discard_saved_state(&saved);

        parser_advance(parser); // consume '('
        value = parser_parse_expression(parser);
        if (!value) {
          return NULL;
        }
        size_t capacity = 4;
        values = calloc(capacity, sizeof(ASTNode *));
        if (!values) {
          ast_destroy_node(value);
          return NULL;
        }
        values[value_count++] = value;
        while (parser->current_token.type == TOKEN_COMMA) {
          parser_advance(parser);
          ASTNode *next = parser_parse_expression(parser);
          if (!next) {
            for (size_t i = 0; i < value_count; i++) {
              ast_destroy_node(values[i]);
            }
            free(values);
            return NULL;
          }
          if (value_count == capacity) {
            capacity *= 2;
            ASTNode **grown = realloc(values, capacity * sizeof(ASTNode *));
            if (!grown) {
              ast_destroy_node(next);
              for (size_t i = 0; i < value_count; i++) {
                ast_destroy_node(values[i]);
              }
              free(values);
              return NULL;
            }
            values = grown;
          }
          values[value_count++] = next;
        }
        if (!parser_expect(parser, TOKEN_RPAREN)) {
          for (size_t i = 0; i < value_count; i++) {
            ast_destroy_node(values[i]);
          }
          free(values);
          return NULL;
        }
      }
    } else {
      value = parser_parse_expression(parser);
    }
  }

  ASTNode *return_stmt = ast_create_node(AST_RETURN_STATEMENT, location);
  if (return_stmt && value) {
    ReturnStatement *ret_data = malloc(sizeof(ReturnStatement));
    if (!ret_data) {
      ast_destroy_node(value);
      free(values);
      free(return_stmt);
      return NULL;
    }
    if (value_count == 0) {
      values = malloc(sizeof(ASTNode *));
      if (!values) {
        ast_destroy_node(value);
        free(ret_data);
        free(return_stmt);
        return NULL;
      }
      values[0] = value;
      value_count = 1;
    }
    ret_data->value = values[0];
    ret_data->values = values;
    ret_data->value_count = value_count;
    return_stmt->data = ret_data;
    for (size_t i = 0; i < value_count; i++) {
      ast_add_child(return_stmt, values[i]);
    }
  }

  return return_stmt;
}

ASTNode *parser_parse_if_statement(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);

  if (!parser_expect(parser, TOKEN_IF)) {
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_refine_error(parser, "Expected '(' after 'if'");
    return NULL;
  }

  parser->group_context = "condition of 'if'";
  ASTNode *condition = parser_parse_expression(parser);
  if (!condition) {
    return NULL;
  }

  parser->group_context = "condition of 'if'";
  if (!parser_expect(parser, TOKEN_RPAREN)) {
    parser_set_error(parser, "Expected ')' after if condition");
    ast_destroy_node(condition);
    return NULL;
  }

  ASTNode *then_branch = (parser->current_token.type == TOKEN_LBRACE)
                             ? parser_parse_block(parser)
                             : parser_parse_statement(parser);
  if (!then_branch) {
    ast_destroy_node(condition);
    return NULL;
  }

  while (parser->current_token.type == TOKEN_NEWLINE) {
    parser_advance(parser);
  }

  ElseIfClause *else_ifs = NULL;
  size_t else_if_count = 0;
  ASTNode *else_branch = NULL;

  while (parser->current_token.type == TOKEN_ELSE) {
    if (parser->peek_token.type == TOKEN_IF) {
      parser_advance(parser); // consume 'else'
      parser_advance(parser); // consume 'if'

      if (!parser_expect(parser, TOKEN_LPAREN)) {
        parser_set_error(parser, "Expected '(' after 'else if'");
        goto cleanup;
      }

      parser->group_context = "condition of 'else if'";
      ASTNode *elif_cond = parser_parse_expression(parser);
      if (!elif_cond)
        goto cleanup;

      parser->group_context = "condition of 'else if'";
      if (!parser_expect(parser, TOKEN_RPAREN)) {
        parser_set_error(parser, "Expected ')' after else if condition");
        ast_destroy_node(elif_cond);
        goto cleanup;
      }

      ASTNode *elif_body = (parser->current_token.type == TOKEN_LBRACE)
                               ? parser_parse_block(parser)
                               : parser_parse_statement(parser);
      if (!elif_body) {
        ast_destroy_node(elif_cond);
        goto cleanup;
      }

      else_ifs = realloc(else_ifs, (else_if_count + 1) * sizeof(ElseIfClause));
      else_ifs[else_if_count].condition = elif_cond;
      else_ifs[else_if_count].body = elif_body;
      else_if_count++;

      while (parser->current_token.type == TOKEN_NEWLINE) {
        parser_advance(parser);
      }
    } else {
      parser_advance(parser); // consume 'else'
      else_branch = (parser->current_token.type == TOKEN_LBRACE)
                        ? parser_parse_block(parser)
                        : parser_parse_statement(parser);
      if (!else_branch)
        goto cleanup;
      break;
    }
  }

  ASTNode *if_node = ast_create_node(AST_IF_STATEMENT, location);
  if (!if_node)
    goto cleanup;

  IfStatement *if_data = malloc(sizeof(IfStatement));
  if (!if_data) {
    ast_destroy_node(if_node);
    goto cleanup;
  }

  if_data->condition = condition;
  if_data->then_branch = then_branch;
  if_data->else_ifs = else_ifs;
  if_data->else_if_count = else_if_count;
  if_data->else_branch = else_branch;
  if_node->data = if_data;

  ast_add_child(if_node, condition);
  ast_add_child(if_node, then_branch);
  for (size_t i = 0; i < else_if_count; i++) {
    ast_add_child(if_node, else_ifs[i].condition);
    ast_add_child(if_node, else_ifs[i].body);
  }
  if (else_branch)
    ast_add_child(if_node, else_branch);

  return if_node;

cleanup:
  ast_destroy_node(condition);
  ast_destroy_node(then_branch);
  for (size_t i = 0; i < else_if_count; i++) {
    ast_destroy_node(else_ifs[i].condition);
    ast_destroy_node(else_ifs[i].body);
  }
  free(else_ifs);
  if (else_branch)
    ast_destroy_node(else_branch);
  return NULL;
}

ASTNode *parser_parse_while_statement(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);

  if (!parser_expect(parser, TOKEN_WHILE)) {
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_refine_error(parser, "Expected '(' after 'while'");
    return NULL;
  }

  parser->group_context = "condition of 'while'";
  ASTNode *condition = parser_parse_expression(parser);
  if (!condition) {
    return NULL;
  }

  parser->group_context = "condition of 'while'";
  if (!parser_expect(parser, TOKEN_RPAREN)) {
    parser_set_error(parser, "Expected ')' after while condition");
    ast_destroy_node(condition);
    return NULL;
  }

  ASTNode *body = (parser->current_token.type == TOKEN_LBRACE)
                      ? parser_parse_block(parser)
                      : parser_parse_statement(parser);
  if (!body) {
    ast_destroy_node(condition);
    return NULL;
  }

  ASTNode *while_node = ast_create_node(AST_WHILE_STATEMENT, location);
  if (!while_node) {
    ast_destroy_node(condition);
    ast_destroy_node(body);
    return NULL;
  }

  WhileStatement *while_data = malloc(sizeof(WhileStatement));
  if (!while_data) {
    ast_destroy_node(while_node);
    ast_destroy_node(condition);
    ast_destroy_node(body);
    return NULL;
  }

  while_data->condition = condition;
  while_data->body = body;
  while_data->label = NULL;
  while_data->simd_mode = SIMD_ATTR_NONE;
  while_data->unroll_factor = 0;
  while_node->data = while_data;

  ast_add_child(while_node, condition);
  ast_add_child(while_node, body);

  return while_node;
}

static ASTNode *parser_parse_break_or_continue(Parser *parser,
                                               TokenType token_type) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, token_type)) {
    return NULL;
  }

  char *target_label = NULL;
  if (parser->current_token.type == TOKEN_IDENTIFIER) {
    target_label = strdup(parser->current_token.value);
    parser_advance(parser);
  }

  parser_expect_statement_end(parser);
  ASTNode *node = (token_type == TOKEN_BREAK)
                      ? ast_create_labeled_break_statement(target_label, location)
                      : ast_create_labeled_continue_statement(target_label, location);
  free(target_label);
  return node;
}

ASTNode *parser_parse_break_statement(Parser *parser) {
  return parser_parse_break_or_continue(parser, TOKEN_BREAK);
}

ASTNode *parser_parse_continue_statement(Parser *parser) {
  return parser_parse_break_or_continue(parser, TOKEN_CONTINUE);
}

// Range-based for: `for i [: type] in start ..|..= end { body }`.
// Desugars at parse time into a classic counted ForStatement so every
// downstream stage (type checker, IR lowering, the counted-loop vectorizer)
// sees the exact shape it already handles:
//
//   for i in lo..hi   =>  var i = lo;  i <  hi;  i = i + 1
//   for i in lo..=hi  =>  var i = lo;  i <= hi;  i = i + 1
//
// The `start` bound is evaluated once (the loop initializer); the `end` bound
// is re-evaluated in the condition each iteration, matching Mettle's C-style
// `for`/`while` semantics. Hoist a call-valued bound yourself if that matters.
// `in` is a contextual keyword (a plain identifier elsewhere), so adding this
// form breaks no existing program that uses `in` as a name.
/* Builds the block above. Takes ownership of `var_name`, `type_name` and
 * `subject` whatever happens, so the caller is done with all three. */
static ASTNode *parser_finish_string_for(Parser *parser, SourceLocation location,
                                         char *var_name, char *type_name,
                                         ASTNode *subject) {
  static int serial = 0;
  char subject_name[32];
  char index_name[32];
  ASTNode *block = NULL;
  ASTNode *subject_decl = NULL;
  ASTNode *index_decl = NULL;
  ASTNode *condition = NULL;
  ASTNode *increment = NULL;
  ASTNode *element_decl = NULL;
  ASTNode *body = NULL;
  ASTNode *loop = NULL;
  int subject_is_named = 0;

  serial++;
  snprintf(index_name, sizeof(index_name), ".fori%d", serial);
  /* A subject that is already a name needs no home of its own: naming it is
   * the whole of evaluating it. Anything else -- a call, a concatenation --
   * gets a hidden local so `for c in read_line(buf)` reads one line rather
   * than one per character. A body that reassigns the named subject is
   * therefore observed by the loop, the same way the range form re-reads its
   * bound each iteration. */
  if (subject && subject->type == AST_IDENTIFIER) {
    Identifier *named = (Identifier *)subject->data;
    if (named && named->name &&
        strlen(named->name) < sizeof(subject_name)) {
      snprintf(subject_name, sizeof(subject_name), "%s", named->name);
      ast_destroy_node(subject);
      subject = NULL;
      subject_is_named = 1;
    }
  }
  if (!subject_is_named) {
    snprintf(subject_name, sizeof(subject_name), ".fors%d", serial);
  }

  body = (parser->current_token.type == TOKEN_LBRACE)
             ? parser_parse_block(parser)
             : parser_parse_statement(parser);
  if (!body) {
    goto fail;
  }

  if (!subject_is_named) {
    subject_decl =
        ast_create_var_declaration(subject_name, NULL, subject, location);
    if (!subject_decl) {
      goto fail;
    }
    subject = NULL;
    ((VarDeclaration *)subject_decl->data)->structural_type = 1;
  }

  /* var c[: type] = .fors[.fori];  -- the element, named as the program asked.
   * Its type is structural when unannotated: it is whatever indexing answers,
   * which for a string is `char`. */
  element_decl = ast_create_var_declaration(
      var_name, type_name,
      ast_create_array_index_expression(
          ast_create_identifier(subject_name, location),
          ast_create_identifier(index_name, location), location),
      location);
  if (!element_decl) {
    goto fail;
  }
  if (!type_name) {
    ((VarDeclaration *)element_decl->data)->structural_type = 1;
  }

  /* The element declaration has to run before the body, and the body may be a
   * single statement rather than a block, so wrap both either way. */
  {
    ASTNode *inner = ast_create_program();
    Program *inner_data = inner ? (Program *)inner->data : NULL;
    ASTNode **grown =
        inner_data ? realloc(inner_data->declarations, 2 * sizeof(ASTNode *))
                   : NULL;
    if (!grown) {
      if (inner) {
        ast_destroy_node(inner);
      }
      goto fail;
    }
    inner_data->declarations = grown;
    inner_data->declarations[0] = element_decl;
    inner_data->declarations[1] = body;
    inner_data->declaration_count = 2;
    ast_add_child(inner, element_decl);
    ast_add_child(inner, body);
    element_decl = NULL;
    body = inner;
  }

  index_decl = ast_create_var_declaration(
      index_name, NULL, ast_create_number_literal(0, location, 10), location);
  if (!index_decl) {
    goto fail;
  }
  ((VarDeclaration *)index_decl->data)->structural_type = 1;

  /* .fori < (int64).fors.length -- the cast keeps the counter's signed
   * comparison from meeting the unsigned length. */
  condition = ast_create_binary_expression(
      ast_create_identifier(index_name, location), "<",
      ast_create_cast_expression(
          "int64",
          ast_create_member_access(
              ast_create_identifier(subject_name, location), "length",
              location),
          location),
      location);
  increment = ast_create_assignment(
      index_name,
      ast_create_binary_expression(
          ast_create_identifier(index_name, location), "+",
          ast_create_number_literal(1, location, 10), location),
      location);
  if (!condition || !increment) {
    goto fail;
  }

  loop = ast_create_for_statement(index_decl, condition, increment, body,
                                  location);
  if (!loop) {
    goto fail;
  }
  index_decl = NULL;
  condition = NULL;
  increment = NULL;
  body = NULL;

  if (subject_is_named) {
    free(var_name);
    free(type_name);
    return loop;
  }

  block = ast_create_program();
  {
    Program *block_data = block ? (Program *)block->data : NULL;
    ASTNode **grown =
        block_data ? realloc(block_data->declarations, 2 * sizeof(ASTNode *))
                   : NULL;
    if (!grown) {
      goto fail;
    }
    block_data->declarations = grown;
    block_data->declarations[0] = subject_decl;
    block_data->declarations[1] = loop;
    block_data->declaration_count = 2;
    ast_add_child(block, subject_decl);
    ast_add_child(block, loop);
  }

  free(var_name);
  free(type_name);
  return block;

fail:
  if (!parser->has_error) {
    parser_set_error(parser, "Out of memory desugaring 'for' over a string");
  }
  free(var_name);
  free(type_name);
  if (subject) {
    ast_destroy_node(subject);
  }
  if (subject_decl) {
    ast_destroy_node(subject_decl);
  }
  if (element_decl) {
    ast_destroy_node(element_decl);
  }
  if (index_decl) {
    ast_destroy_node(index_decl);
  }
  if (condition) {
    ast_destroy_node(condition);
  }
  if (increment) {
    ast_destroy_node(increment);
  }
  if (body) {
    ast_destroy_node(body);
  }
  if (loop) {
    ast_destroy_node(loop);
  }
  if (block) {
    ast_destroy_node(block);
  }
  return NULL;
}

static ASTNode *parser_parse_range_for(Parser *parser, SourceLocation location) {
  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected '(' or a loop variable after 'for'");
    return NULL;
  }
  char *var_name = strdup(parser->current_token.value);
  parser_advance(parser);

  // Optional type annotation: `for i: int64 in ...`. Without one, the variable
  // type is inferred from the start bound (same rule as `var i = <expr>`).
  char *type_name = NULL;
  if (parser->current_token.type == TOKEN_COLON) {
    parser_advance(parser);
    type_name = parser_parse_type_annotation(parser);
    if (!type_name) {
      if (!parser->has_error)
        parser_set_error(parser, "Expected type after ':'");
      free(var_name);
      return NULL;
    }
  }

  // Contextual `in`.
  if (!parser_is_identifier_like(parser->current_token.type) ||
      strcmp(parser->current_token.value, "in") != 0) {
    // `for i = 0; ...` is the C header written without its parentheses. Name
    // the two forms Mettle has rather than complaining about a missing 'in'
    // the reader never meant to write.
    if (parser->current_token.type == TOKEN_EQUALS) {
      char help[PARSER_ERROR_BUF_SIZE];
      snprintf(help, sizeof(help),
               "write the range as 'for %s in 0..n { ... }', or the counted "
               "header inside parentheses as 'for (var %s: int64 = 0; %s < n; "
               "%s = %s + 1) { ... }'",
               var_name, var_name, var_name, var_name, var_name);
      parser_set_error_with_suggestion(
          parser, "A 'for' header needs 'in' or parentheses", help);
      parser->recover_at_body_brace = 1;
    } else {
      parser_set_error(parser, "Expected 'in' in range-based for loop");
    }
    free(var_name);
    free(type_name);
    return NULL;
  }
  parser_advance(parser);

  ASTNode *start = parser_parse_expression(parser);
  if (!start) {
    free(var_name);
    free(type_name);
    return NULL;
  }

  /* `for c in s` over a string. No '..' followed the subject, so this is the
   * character form rather than a range, and it desugars into the counted loop
   * the rest of the compiler already handles:
   *
   *   {
   *     var .fors: string = s;
   *     for .fori in 0 .. (int64).fors.length {
   *       var c: char = .fors[.fori];
   *       <body>
   *     }
   *   }
   *
   * The subject gets a hidden local so it is evaluated once: `for c in
   * read_line(buf)` must not read a line per character. The hidden names start
   * with a dot, which no source identifier can, so neither can collide with a
   * name the program chose or with a nested loop's own pair. */
  if (parser->current_token.type != TOKEN_DOT_DOT) {
    return parser_finish_string_for(parser, location, var_name, type_name,
                                    start);
  }
  parser_advance(parser); // consume '..'
  int inclusive = 0;
  if (parser->current_token.type == TOKEN_EQUALS) {
    inclusive = 1;
    parser_advance(parser); // consume '=' of '..='
  }

  ASTNode *end = parser_parse_expression(parser);
  if (!end) {
    free(var_name);
    free(type_name);
    ast_destroy_node(start);
    return NULL;
  }

  ASTNode *body = (parser->current_token.type == TOKEN_LBRACE)
                      ? parser_parse_block(parser)
                      : parser_parse_statement(parser);
  if (!body) {
    free(var_name);
    free(type_name);
    ast_destroy_node(start);
    ast_destroy_node(end);
    return NULL;
  }

  // initializer: var <name>[: type] = <start>
  ASTNode *initializer =
      ast_create_var_declaration(var_name, type_name, start, location);
  // The loop counter's type is structural (it takes the type of its bound), so
  // it is exempt from the "explicit type required" rule when no `: type` given.
  if (initializer && !type_name) {
    ((VarDeclaration *)initializer->data)->structural_type = 1;
  }
  // condition: <name> <  <end>   (exclusive)
  //            <name> <= <end>   (inclusive)
  ASTNode *condition = ast_create_binary_expression(
      ast_create_identifier(var_name, location), inclusive ? "<=" : "<", end,
      location);
  // increment: <name> = <name> + 1
  ASTNode *step_value = ast_create_binary_expression(
      ast_create_identifier(var_name, location), "+",
      ast_create_number_literal(1, location, 10), location);
  ASTNode *increment = ast_create_assignment(var_name, step_value, location);

  free(var_name);
  free(type_name);

  if (!initializer || !condition || !increment) {
    if (initializer)
      ast_destroy_node(initializer);
    if (condition)
      ast_destroy_node(condition);
    if (increment)
      ast_destroy_node(increment);
    ast_destroy_node(body);
    parser_set_error(parser, "Out of memory desugaring range-based for loop");
    return NULL;
  }

  return ast_create_for_statement(initializer, condition, increment, body,
                                  location);
}

// GPU kernel launch. The compact form remains
//
//   dispatch K[grid, block](a0, ...)
//
// and the complete neutral launch surface is
//
//   dispatch K[grid: (gx, gy, gz), block: (bx, by, bz),
//              shared: bytes, stream: handle](a0, ...)
//
// Named options may be reordered; grid and block are required, while shared
// and stream default to zero. Parsing preserves a semantic launch statement.
// Argument ABI marshalling belongs to host-runtime lowering, not the parser.
static ASTNode *parser_parse_dispatch_statement(Parser *parser) {
  SourceLocation loc = parser_current_location(parser);
  parser_advance(parser); // consume 'dispatch'

  if (parser->current_token.type != TOKEN_IDENTIFIER) {
    parser_set_error(parser,
                     "Expected a kernel handle (identifier) after 'dispatch'");
    return NULL;
  }
  ASTNode *kernel =
      ast_create_identifier(parser->current_token.value, loc);
  parser_advance(parser);

  ASTNode *grid[3] = {NULL, NULL, NULL};
  ASTNode *block[3] = {NULL, NULL, NULL};
  ASTNode *shared = NULL;
  ASTNode *stream = NULL;
  ASTNode *work = NULL;
  int stream_was_named = 0;
  ASTNode *args[64];
  size_t nargs = 0;

#define DISP_FAIL()                                                            \
  do {                                                                         \
    ast_destroy_node(kernel);                                                  \
    for (size_t _d = 0; _d < 3; _d++) {                                       \
      ast_destroy_node(grid[_d]);                                              \
      ast_destroy_node(block[_d]);                                             \
    }                                                                          \
    ast_destroy_node(shared);                                                  \
    ast_destroy_node(stream);                                                  \
    ast_destroy_node(work);                                                    \
    for (size_t _i = 0; _i < nargs; _i++)                                      \
      ast_destroy_node(args[_i]);                                              \
    return NULL;                                                               \
  } while (0)

  if (!kernel) {
    parser_set_error(parser, "Out of memory creating GPU launch handle");
    DISP_FAIL();
  }

  // Compact [grid, block] or complete named launch controls.
  if (!parser_expect(parser, TOKEN_LBRACKET)) {
    parser_set_error(parser, "Expected launch controls after the dispatch kernel");
    DISP_FAIL();
  }
  int named_controls = parser->current_token.type == TOKEN_IDENTIFIER &&
                       parser->peek_token.type == TOKEN_COLON;
  if (!named_controls) {
    grid[0] = parser_parse_expression(parser);
    if (!grid[0] || !parser_expect(parser, TOKEN_COMMA)) {
      if (grid[0] && !parser->has_error)
        parser_set_error(parser,
                         "Expected ',' between grid and block in dispatch");
      DISP_FAIL();
    }
    block[0] = parser_parse_expression(parser);
    if (!block[0] || !parser_expect(parser, TOKEN_RBRACKET)) {
      DISP_FAIL();
    }
    grid[1] = ast_create_number_literal(1, loc, 10);
    grid[2] = ast_create_number_literal(1, loc, 10);
    block[1] = ast_create_number_literal(1, loc, 10);
    block[2] = ast_create_number_literal(1, loc, 10);
  } else {
    int have_grid = 0, have_block = 0, have_shared = 0, have_stream = 0;
    while (parser->current_token.type != TOKEN_RBRACKET) {
      if (parser->current_token.type != TOKEN_IDENTIFIER ||
          parser->peek_token.type != TOKEN_COLON) {
        parser_set_error(
            parser,
            "Expected grid:, block:, shared:, or stream: in dispatch controls");
        DISP_FAIL();
      }
      int option = 0;
      if (!strcmp(parser->current_token.value, "grid")) option = 1;
      else if (!strcmp(parser->current_token.value, "block")) option = 2;
      else if (!strcmp(parser->current_token.value, "shared")) option = 3;
      else if (!strcmp(parser->current_token.value, "stream")) option = 4;
      else if (!strcmp(parser->current_token.value, "work")) option = 5;
      else {
        parser_set_error(parser, "Unknown named dispatch control");
        DISP_FAIL();
      }
      if ((option == 1 && have_grid) || (option == 2 && have_block) ||
          (option == 3 && have_shared) || (option == 4 && have_stream) ||
          (option == 5 && work)) {
        parser_set_error(parser, "Duplicate named dispatch control");
        DISP_FAIL();
      }
      if (option == 1) have_grid = 1;
      else if (option == 2) have_block = 1;
      else if (option == 3) have_shared = 1;
      else if (option == 5) { /* `work` is recorded by its parse below */ }
      else {
        have_stream = 1;
        stream_was_named = 1;
      }

      parser_advance(parser); // control name
      if (!parser_expect(parser, TOKEN_COLON)) DISP_FAIL();
      if (option == 1 || option == 2) {
        ASTNode **dimensions = option == 1 ? grid : block;
        if (!parser_expect(parser, TOKEN_LPAREN)) {
          parser_set_error(parser,
                           "Expected a three-dimensional dispatch tuple");
          DISP_FAIL();
        }
        for (size_t d = 0; d < 3; d++) {
          dimensions[d] = parser_parse_expression(parser);
          if (!dimensions[d]) DISP_FAIL();
          if (d < 2 && !parser_expect(parser, TOKEN_COMMA)) {
            parser_set_error(parser,
                             "Dispatch grid and block require exactly three dimensions");
            DISP_FAIL();
          }
        }
        if (!parser_expect(parser, TOKEN_RPAREN)) {
          parser_set_error(parser,
                           "Dispatch grid and block require exactly three dimensions");
          DISP_FAIL();
        }
      } else {
        ASTNode **value = option == 3 ? &shared
                                      : (option == 5 ? &work : &stream);
        *value = parser_parse_expression(parser);
        if (!*value) DISP_FAIL();
      }
      if (parser->current_token.type == TOKEN_COMMA) {
        parser_advance(parser);
        if (parser->current_token.type == TOKEN_RBRACKET) {
          parser_set_error(parser,
                           "Trailing comma in named dispatch controls");
          DISP_FAIL();
        }
      } else if (parser->current_token.type != TOKEN_RBRACKET) {
        parser_set_error(parser,
                         "Expected ',' between named dispatch controls");
        DISP_FAIL();
      }
    }
    /* `work:` replaces both: the grid comes from the work count divided by
     * the kernel's declared block shape, which the type checker supplies. */
    if (work) {
      if (have_grid) {
        parser_set_error(parser,
                         "Dispatch 'work:' computes the grid; drop 'grid:'");
        DISP_FAIL();
      }
    } else if (!have_grid || !have_block) {
      parser_set_error(parser,
                       "Named dispatch controls require grid and block, or "
                       "'work:' to size the launch from the kernel's "
                       "declared block");
      DISP_FAIL();
    }
    if (!parser_expect(parser, TOKEN_RBRACKET)) DISP_FAIL();
  }

  if (!shared) shared = ast_create_number_literal(0, loc, 10);
  if (!stream) stream = ast_create_number_literal(0, loc, 10);
  /* A `work:` launch carries placeholder geometry: lowering replaces it with
   * the work count divided by the kernel's declared block shape. */
  if (work) {
    for (size_t d = 0; d < 3; d++) {
      if (!grid[d]) grid[d] = ast_create_number_literal(1, loc, 10);
      if (!block[d]) block[d] = ast_create_number_literal(1, loc, 10);
    }
  }
  if (!grid[1] || !grid[2] || !block[1] || !block[2] || !shared || !stream) {
    parser_set_error(parser, "Out of memory creating GPU launch controls");
    DISP_FAIL();
  }

  // ( args )
  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_set_error(parser, "Expected '(' before dispatch arguments");
    DISP_FAIL();
  }
  if (parser->current_token.type != TOKEN_RPAREN) {
    while (1) {
      if (nargs >= 64) {
        parser_set_error(parser, "too many dispatch arguments (max 64)");
        DISP_FAIL();
      }
      ASTNode *a = parser_parse_expression(parser);
      if (!a) {
        DISP_FAIL();
      }
      args[nargs++] = a;
      if (parser->current_token.type == TOKEN_COMMA) {
        parser_advance(parser);
        continue;
      }
      break;
    }
  }
  if (!parser_expect(parser, TOKEN_RPAREN)) {
    DISP_FAIL();
  }

  /* `dispatch K[grid, block](args) on s;` -- enqueue on an explicit stream.
   * Sugar over the named `stream:` control, so the compact form can overlap
   * launches with transfers without spelling the full three-dimensional
   * geometry. */
  if (parser->current_token.type == TOKEN_IDENTIFIER &&
      strcmp(parser->current_token.value, "on") == 0) {
    if (stream_was_named) {
      parser_set_error(parser,
                       "Dispatch stream is already given by the named "
                       "'stream:' control; drop one of the two");
      DISP_FAIL();
    }
    parser_advance(parser); // consume 'on'
    ast_destroy_node(stream);
    stream = parser_parse_expression(parser);
    if (!stream) {
      if (!parser->has_error)
        parser_set_error(parser, "Expected a stream expression after 'on'");
      DISP_FAIL();
    }
  }

  ASTNode *launch = ast_create_gpu_launch(kernel, grid, block, shared, stream,
                                          args, nargs, loc);
  if (!launch) {
    parser_set_error(parser, "Out of memory creating GPU launch");
    DISP_FAIL();
  }
  if (work) {
    GpuLaunchStatement *data = (GpuLaunchStatement *)launch->data;
    data->work = work;
    ast_add_child(launch, work);
  }
#undef DISP_FAIL
  return launch;
}

ASTNode *parser_parse_for_statement(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, TOKEN_FOR)) {
    return NULL;
  }

  // The classic C-style form always opens with '('. Anything else starts a
  // range-based loop: `for i in lo..hi { ... }`.
  if (parser->current_token.type != TOKEN_LPAREN) {
    return parser_parse_range_for(parser, location);
  }

  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_set_error(parser, "Expected '(' after 'for'");
    return NULL;
  }

  ASTNode *initializer = parser_parse_for_initializer(parser);
  if (parser->has_error) {
    if (initializer)
      ast_destroy_node(initializer);
    return NULL;
  }

  if (parser->current_token.type == TOKEN_SEMICOLON) {
    parser_advance(parser);
  } else if (!initializer || initializer->type != AST_VAR_DECLARATION) {
    parser_set_error(parser, "Expected ';' after for-loop initializer");
    if (initializer)
      ast_destroy_node(initializer);
    return NULL;
  }

  ASTNode *condition = NULL;
  if (parser->current_token.type != TOKEN_SEMICOLON) {
    condition = parser_parse_expression(parser);
    if (!condition) {
      if (initializer)
        ast_destroy_node(initializer);
      return NULL;
    }
  }

  if (!parser_expect(parser, TOKEN_SEMICOLON)) {
    if (initializer)
      ast_destroy_node(initializer);
    if (condition)
      ast_destroy_node(condition);
    return NULL;
  }

  ASTNode *increment = NULL;
  if (parser->current_token.type != TOKEN_RPAREN) {
    ASTNode *expr = parser_parse_expression(parser);
    if (!expr) {
      if (initializer)
        ast_destroy_node(initializer);
      if (condition)
        ast_destroy_node(condition);
      return NULL;
    }

    if (parser_is_assignment_token(parser->current_token.type)) {
      increment = parser_parse_assignment_from_target(parser, expr);
      if (!increment) {
        if (initializer)
          ast_destroy_node(initializer);
        if (condition)
          ast_destroy_node(condition);
        return NULL;
      }
    } else {
      increment = expr;
    }
  }

  if (!parser_expect(parser, TOKEN_RPAREN)) {
    if (initializer)
      ast_destroy_node(initializer);
    if (condition)
      ast_destroy_node(condition);
    if (increment)
      ast_destroy_node(increment);
    return NULL;
  }

  ASTNode *body = (parser->current_token.type == TOKEN_LBRACE)
                      ? parser_parse_block(parser)
                      : parser_parse_statement(parser);
  if (!body) {
    if (initializer)
      ast_destroy_node(initializer);
    if (condition)
      ast_destroy_node(condition);
    if (increment)
      ast_destroy_node(increment);
    return NULL;
  }

  return ast_create_for_statement(initializer, condition, increment, body,
                                  location);
}

ASTNode *parser_parse_switch_statement(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, TOKEN_SWITCH)) {
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_set_error(parser, "Expected '(' after 'switch'");
    return NULL;
  }

  parser->group_context = "subject of 'switch'";
  ASTNode *expression = parser_parse_expression(parser);
  if (!expression) {
    return NULL;
  }

  parser->group_context = "subject of 'switch'";
  if (!parser_expect(parser, TOKEN_RPAREN)) {
    ast_destroy_node(expression);
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_LBRACE)) {
    ast_destroy_node(expression);
    return NULL;
  }

  ASTNode **cases = NULL;
  size_t case_count = 0;
  int seen_default = 0;

  while (parser->current_token.type != TOKEN_RBRACE &&
         parser->current_token.type != TOKEN_EOF) {
    while (parser->current_token.type == TOKEN_NEWLINE ||
           parser->current_token.type == TOKEN_SEMICOLON) {
      parser_advance(parser);
    }

    if (parser->current_token.type == TOKEN_RBRACE) {
      break;
    }

    int is_default = 0;
    ASTNode *case_value = NULL;
    ASTNode *case_high = NULL;
    SourceLocation case_loc = parser_current_location(parser);

    if (parser->current_token.type == TOKEN_CASE) {
      parser_advance(parser);
      case_value = parser_parse_expression(parser);
      if (!case_value) {
        parser_set_error(parser, "Expected constant expression after 'case'");
        break;
      }
      // Range case: `case lo..hi:` matches any value in [lo, hi].
      if (parser->current_token.type == TOKEN_DOT_DOT) {
        parser_advance(parser);
        case_high = parser_parse_expression(parser);
        if (!case_high) {
          parser_set_error(parser,
                           "Expected upper bound expression after '..'");
          ast_destroy_node(case_value);
          break;
        }
      }
    } else if (parser->current_token.type == TOKEN_DEFAULT) {
      if (seen_default) {
        parser_set_error(parser, "Only one default case is allowed");
        break;
      }
      seen_default = 1;
      is_default = 1;
      parser_advance(parser);
    } else {
      parser_set_error(parser, "Expected 'case' or 'default' in switch");
      break;
    }

    if (!parser_expect(parser, TOKEN_COLON)) {
      if (case_value)
        ast_destroy_node(case_value);
      if (case_high)
        ast_destroy_node(case_high);
      break;
    }

    ASTNode *case_body = ast_create_program();
    if (!case_body) {
      if (case_value)
        ast_destroy_node(case_value);
      if (case_high)
        ast_destroy_node(case_high);
      parser_set_error(parser, "Memory allocation failed for switch case");
      break;
    }

    Program *body_prog = (Program *)case_body->data;
    while (parser->current_token.type != TOKEN_EOF &&
           parser->current_token.type != TOKEN_CASE &&
           parser->current_token.type != TOKEN_DEFAULT &&
           parser->current_token.type != TOKEN_RBRACE) {
      if (parser->current_token.type == TOKEN_NEWLINE ||
          parser->current_token.type == TOKEN_SEMICOLON) {
        parser_advance(parser);
        continue;
      }

      ASTNode *stmt = parser_parse_statement(parser);
      if (!stmt) {
        ast_destroy_node(case_body);
        if (case_value)
          ast_destroy_node(case_value);
        if (case_high)
          ast_destroy_node(case_high);
        for (size_t i = 0; i < case_count; i++)
          ast_destroy_node(cases[i]);
        free(cases);
        ast_destroy_node(expression);
        return NULL;
      }

      body_prog->declarations =
          realloc(body_prog->declarations,
                  (body_prog->declaration_count + 1) * sizeof(ASTNode *));
      body_prog->declarations[body_prog->declaration_count++] = stmt;
      ast_add_child(case_body, stmt);
    }

    ASTNode *case_node =
        ast_create_case_clause(case_value, case_body, is_default, case_loc);
    if (!case_node) {
      ast_destroy_node(case_body);
      if (case_value)
        ast_destroy_node(case_value);
      if (case_high)
        ast_destroy_node(case_high);
      parser_set_error(parser, "Failed to create switch case clause");
      break;
    }

    if (case_high) {
      ((CaseClause *)case_node->data)->value_high = case_high;
      ast_add_child(case_node, case_high);
    }

    cases = realloc(cases, (case_count + 1) * sizeof(ASTNode *));
    cases[case_count++] = case_node;
  }

  if (!parser_expect(parser, TOKEN_RBRACE)) {
    for (size_t i = 0; i < case_count; i++)
      ast_destroy_node(cases[i]);
    free(cases);
    ast_destroy_node(expression);
    return NULL;
  }

  ASTNode *switch_node =
      ast_create_switch_statement(expression, cases, case_count, location);
  free(cases);
  return switch_node;
}

// Parse: match (expr) {
//   case VariantName(binding): { body }   (statement form; body is a block)
//   case VariantName: { body }
//   default: { body }
// }
// In expression form (is_expression=1) each arm body is a value-yielding
// expression instead of a block:
//   match (expr) { case Some(v): v + 1, default: 0 }
static ASTNode *parser_parse_match_core(Parser *parser, int is_expression) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  if (!parser_expect(parser, TOKEN_MATCH))
    return NULL;

  if (!parser_expect(parser, TOKEN_LPAREN)) {
    parser_refine_error(parser, "Expected '(' after 'match'");
    return NULL;
  }
  ASTNode *expr = parser_parse_expression(parser);
  if (!expr)
    return NULL;
  if (!parser_expect(parser, TOKEN_RPAREN)) {
    ast_destroy_node(expr);
    return NULL;
  }
  if (!parser_expect(parser, TOKEN_LBRACE)) {
    ast_destroy_node(expr);
    return NULL;
  }

  MatchArm *arms = NULL;
  size_t arm_count = 0;
  int seen_default = 0;

  while (parser->current_token.type != TOKEN_RBRACE &&
         parser->current_token.type != TOKEN_EOF && !parser->has_error) {
    while (parser->current_token.type == TOKEN_NEWLINE ||
           parser->current_token.type == TOKEN_SEMICOLON)
      parser_advance(parser);
    if (parser->current_token.type == TOKEN_RBRACE)
      break;

    MatchArm arm = {0};

    if (parser->current_token.type == TOKEN_DEFAULT) {
      if (seen_default) {
        parser_set_error(parser, "Only one default arm is allowed in match");
        break;
      }
      seen_default = 1;
      arm.is_default = 1;
      arm.variant_name = strdup("default");
      parser_advance(parser);
    } else if (parser->current_token.type == TOKEN_CASE) {
      parser_advance(parser); // consume 'case'
      if (!parser_is_identifier_like(parser->current_token.type)) {
        parser_set_error(parser, "Expected variant name after 'case' in match");
        break;
      }
      arm.variant_name = strdup(parser->current_token.value);
      parser_advance(parser);

      // Optional binding: case Some(v)
      if (parser->current_token.type == TOKEN_LPAREN) {
        parser_advance(parser); // consume '('
        if (!parser_is_identifier_like(parser->current_token.type)) {
          parser_set_error(parser, "Expected binding name in match arm");
          free(arm.variant_name);
          break;
        }
        arm.binding_name = strdup(parser->current_token.value);
        parser_advance(parser);
        if (!parser_expect(parser, TOKEN_RPAREN)) {
          free(arm.binding_name);
          free(arm.variant_name);
          break;
        }
      }
    } else {
      parser_set_error(parser, "Expected 'case' or 'default' in match");
      break;
    }

    if (!parser_expect(parser, TOKEN_COLON)) {
      free(arm.binding_name);
      free(arm.variant_name);
      break;
    }

    if (is_expression) {
      arm.body = parser_parse_expression(parser);
      if (!arm.body) {
        if (!parser->has_error)
          parser_set_error(parser,
                           "Expected value expression in match arm");
        free(arm.binding_name);
        free(arm.variant_name);
        break;
      }
    } else {
      arm.body = parser_parse_block(parser);
      if (!arm.body) {
        if (!parser->has_error)
          parser_set_error(parser, "Expected '{' block body in match arm");
        free(arm.binding_name);
        free(arm.variant_name);
        break;
      }
    }

    MatchArm *new_arms = realloc(arms, (arm_count + 1) * sizeof(MatchArm));
    if (!new_arms) {
      parser_set_error(parser, "Out of memory in match statement");
      ast_destroy_node(arm.body);
      free(arm.binding_name);
      free(arm.variant_name);
      break;
    }
    arms = new_arms;
    arms[arm_count++] = arm;

    // Expression-form arms may be separated by a comma.
    if (is_expression && parser->current_token.type == TOKEN_COMMA)
      parser_advance(parser);

    while (parser->current_token.type == TOKEN_NEWLINE ||
           parser->current_token.type == TOKEN_SEMICOLON)
      parser_advance(parser);
  }

  if (parser->has_error) {
    for (size_t i = 0; i < arm_count; i++) {
      free(arms[i].variant_name);
      free(arms[i].binding_name);
      ast_destroy_node(arms[i].body);
    }
    free(arms);
    ast_destroy_node(expr);
    return NULL;
  }

  if (!parser_expect(parser, TOKEN_RBRACE)) {
    for (size_t i = 0; i < arm_count; i++) {
      free(arms[i].variant_name);
      free(arms[i].binding_name);
      ast_destroy_node(arms[i].body);
    }
    free(arms);
    ast_destroy_node(expr);
    return NULL;
  }

  ASTNode *node =
      is_expression
          ? ast_create_match_expression(expr, arms, arm_count, location)
          : ast_create_match_statement(expr, arms, arm_count, location);
  for (size_t i = 0; i < arm_count; i++) {
    free(arms[i].variant_name);
    free(arms[i].binding_name);
    // body nodes now owned by the match node
  }
  free(arms);
  return node;
}

ASTNode *parser_parse_match_statement(Parser *parser) {
  return parser_parse_match_core(parser, 0);
}

ASTNode *parser_parse_match_expression(Parser *parser) {
  return parser_parse_match_core(parser, 1);
}

ASTNode *parser_parse_block(Parser *parser) {
  if (!parser)
    return NULL;

  // Expect '{'
  if (!parser_expect(parser, TOKEN_LBRACE)) {
    return NULL;
  }

  /* Blocks nest through the statement parser the way expressions nest through
   * the expression parser, and the passes that walk them recurse the same way,
   * so they answer to the same ceiling. brace_depth already counts the braces
   * consumed, so the guard reads it rather than keeping a second tally across
   * this function's several exits. The check follows the '{' rather than
   * preceding it: a refusal that consumed nothing would leave the caller
   * looking at the same token and trying again forever. */
  if (parser->brace_depth >= PARSER_MAX_EXPRESSION_DEPTH) {
    parser_report_block_too_deep(parser);
    return NULL;
  }

  // Create a block node (we'll use a program node to hold statements)
  ASTNode *block = ast_create_program();
  if (!block)
    return NULL;

  Program *block_data = (Program *)block->data;

  // Depth this block's body sits at, so recovery can find its closing brace.
  const int body_depth = parser->brace_depth;

  // Parse statements until we hit '}'
  while (parser->current_token.type != TOKEN_RBRACE &&
         parser->current_token.type != TOKEN_EOF) {

    // Skip empty statements/newlines
    if (parser->current_token.type == TOKEN_NEWLINE ||
        parser->current_token.type == TOKEN_SEMICOLON) {
      parser_advance(parser);
      continue;
    }

    ASTNode *stmt = parser_parse_statement(parser);
    if (stmt) {
      // Add to block's statements array
      block_data->declarations =
          realloc(block_data->declarations,
                  (block_data->declaration_count + 1) * sizeof(ASTNode *));
      if (block_data->declarations) {
        block_data->declarations[block_data->declaration_count] = stmt;
        block_data->declaration_count++;
        ast_add_child(block, stmt);
      }

      // Attempt to consume optional statement end (semicolon or newline)
      // for statements that didn't already consume it.
      if (stmt->type != AST_DEFER_STATEMENT &&
          (parser->current_token.type == TOKEN_SEMICOLON ||
           parser->current_token.type == TOKEN_NEWLINE)) {
        parser_expect_statement_end(parser);
      }
      if (parser->has_error) {
        // A statement parsed but its tail did not. Recover here too, so one
        // stray token does not spill the rest of the body onto file scope.
        parser_recover_in_block(parser, body_depth);
        parser->has_error = 0;
        free(parser->error_message);
        parser->error_message = NULL;
      }
    } else {
      if (!parser->has_error)
        parser_set_error(parser, "Expected a statement");
      // Skip to the next statement in this same block and keep checking. The
      // block still consumes its own '}', so one bad statement costs one
      // diagnostic rather than a run of them from file scope.
      parser_recover_in_block(parser, body_depth);
      parser->has_error = 0;
      free(parser->error_message);
      parser->error_message = NULL;
    }
  }

  // Expect '}'
  if (!parser_expect(parser, TOKEN_RBRACE)) {
    ast_destroy_node(block);
    return NULL;
  }

  return block;
}

ASTNode *parser_parse_method_declaration(Parser *parser) {
  if (!parser)
    return NULL;

  SourceLocation location = parser_current_location(parser);
  // Expect 'method' keyword
  if (!parser_expect(parser, TOKEN_METHOD)) {
    return NULL;
  }

  // Expect method name
  if (!parser_is_identifier_like(parser->current_token.type)) {
    parser_set_error(parser, "Expected method name after 'method'");
    return NULL;
  }

  char *method_name = strdup(parser->current_token.value);
  parser_advance(parser);

  // Expect '('
  if (!parser_expect(parser, TOKEN_LPAREN)) {
    free(method_name);
    return NULL;
  }

  char **param_names = NULL;
  char **param_types = NULL;
  size_t param_count = 0;

  if (!parser_parse_parameter_list(parser, &param_names, &param_types,
                                   &param_count)) {
    free(method_name);
    return NULL;
  }

  // Expect ')'
  if (!parser_expect(parser, TOKEN_RPAREN)) {
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    free(method_name);
    return NULL;
  }

  // Optional return type: '-> type' (or ': type' for compatibility)
  char *return_type = NULL;
  if (parser->current_token.type == TOKEN_ARROW ||
      parser->current_token.type == TOKEN_COLON) {
    parser_advance(parser); // consume return separator

    return_type = parser_parse_type_annotation(parser);
    if (!return_type) {
      if (!parser->has_error) {
        parser_set_error(parser, "Expected return type after return separator");
      }
      for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
        free(param_types[i]);
      }
      free(param_names);
      free(param_types);
      free(method_name);
      return NULL;
    }
  }

  ParsedEffectClauses effect_clauses;
  if (!parser_parse_effect_clauses(parser, &effect_clauses)) {
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    free(method_name);
    free(return_type);
    return NULL;
  }

  // Parse method body (block)
  ASTNode *body = NULL;
  if (parser->current_token.type == TOKEN_LBRACE) {
    body = parser_parse_block(parser);
    if (!body && parser->has_error) {
      // Clean up
      for (size_t i = 0; i < param_count; i++) {
        free(param_names[i]);
        free(param_types[i]);
      }
      free(param_names);
      free(param_types);
      free(method_name);
      free(return_type);
      parser_free_effect_clauses(&effect_clauses);
      return NULL;
    }
  } else {
    parser_set_error(parser, "Expected method body ('{')");
    // Clean up
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    free(method_name);
    free(return_type);
    parser_free_effect_clauses(&effect_clauses);
    return NULL;
  }

  // Create method declaration node (we'll use AST_METHOD_DECLARATION type)
  ASTNode *method_decl = ast_create_node(AST_METHOD_DECLARATION, location);
  if (!method_decl) {
    // Clean up
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    free(method_name);
    free(return_type);
    parser_free_effect_clauses(&effect_clauses);
    if (body)
      ast_destroy_node(body);
    return NULL;
  }

  // Create method declaration data (reuse FunctionDeclaration structure)
  FunctionDeclaration *method_data = calloc(1, sizeof(FunctionDeclaration));
  if (!method_data) {
    // Clean up
    for (size_t i = 0; i < param_count; i++) {
      free(param_names[i]);
      free(param_types[i]);
    }
    free(param_names);
    free(param_types);
    free(method_name);
    free(return_type);
    parser_free_effect_clauses(&effect_clauses);
    if (body)
      ast_destroy_node(body);
    ast_destroy_node(method_decl);
    return NULL;
  }
  memset(method_data, 0, sizeof(FunctionDeclaration));
  if (!parser_apply_effect_clauses(method_data, &effect_clauses)) {
    parser_set_error(parser, "Memory allocation failed for effect clauses");
  }
  parser_free_effect_clauses(&effect_clauses);

  method_data->name = (char *)string_intern(method_name);
  method_data->return_type =
      return_type ? (char *)string_intern(return_type) : NULL;
  method_data->parameter_count = param_count;
  method_data->body = body;

  if (param_count > 0) {
    method_data->parameter_names = malloc(param_count * sizeof(char *));
    method_data->parameter_types = malloc(param_count * sizeof(char *));

    for (size_t i = 0; i < param_count; i++) {
      method_data->parameter_names[i] = (char *)string_intern(param_names[i]);
      method_data->parameter_types[i] = (char *)string_intern(param_types[i]);
    }
  } else {
    method_data->parameter_names = NULL;
    method_data->parameter_types = NULL;
  }

  method_decl->data = method_data;

  if (body) {
    ast_add_child(method_decl, body);
  }

  // Clean up temporary strings
  free(method_name);
  free(return_type);
  for (size_t i = 0; i < param_count; i++) {
    free(param_names[i]);
    free(param_types[i]);
  }
  free(param_names);
  free(param_types);

  return method_decl;
}
