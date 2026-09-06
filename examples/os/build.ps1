param(
  [string]$Compiler = (Join-Path $PSScriptRoot "../../bin/mettle.exe"),
  [string]$Image = (Join-Path $PSScriptRoot "mettleos.vhd"),
  [switch]$Fresh
)

$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$bootSource = Join-Path $here "boot.mettle"
$stageSource = Join-Path $here "stage2.mettle"
$kernelSource = Join-Path $here "kernel.mettle"
$bootImage = Join-Path $here "boot.bin"
$stageImage = Join-Path $here "stage2.bin"
$kernelImage = Join-Path $here "kernel.bin"
$archiveImage = Join-Path $here "files.img"
$fileRoot = Join-Path $here "files"

$sectorSize = 512
$stageLba = 1
$stageSectors = 32
$kernelLba = 64
$kernelSectors = 896
$archiveLba = 1024
$archiveSectors = 1024
$partitionLba = 2048
$diskSectors = 524288

$stageRoom = $stageSectors * $sectorSize
$kernelRoom = $kernelSectors * $sectorSize
$archiveRoom = $archiveSectors * $sectorSize
$systemBytes = $partitionLba * $sectorSize
$dataBytes = $diskSectors * $sectorSize

if (-not (Test-Path $Compiler)) {
  throw "no compiler at $Compiler"
}

& $Compiler $bootSource --target i8086-none --image-base 0x7c00 --emit-flat $bootImage
if ($LASTEXITCODE -ne 0) { throw "the boot sector did not compile" }

& $Compiler $stageSource --target i8086-none --image-base 0x8000 --emit-flat $stageImage
if ($LASTEXITCODE -ne 0) { throw "the second stage did not compile" }

& $Compiler $kernelSource --target x86_64-none --release --image-base 0x20000 --emit-flat $kernelImage
if ($LASTEXITCODE -ne 0) { throw "the kernel did not compile" }

$boot = [System.IO.File]::ReadAllBytes($bootImage)
$stage = [System.IO.File]::ReadAllBytes($stageImage)
$kernel = [System.IO.File]::ReadAllBytes($kernelImage)

if ($boot.Length -ne $sectorSize) {
  throw "the boot sector is $($boot.Length) bytes, not $sectorSize"
}
if ($boot[510] -ne 0x55 -or $boot[511] -ne 0xAA) {
  throw "the boot sector carries no signature"
}
$bootUsed = 446
while ($bootUsed -gt 0 -and $boot[$bootUsed - 1] -eq 0) { $bootUsed-- }
if ($stage.Length -gt $stageRoom) {
  throw "the second stage is $($stage.Length) bytes and the disk reserves $stageRoom"
}
if ($kernel.Length -gt $kernelRoom) {
  throw "the kernel is $($kernel.Length) bytes and the disk reserves $kernelRoom for it"
}

$files = @()
if (Test-Path $fileRoot) {
  $files = @(Get-ChildItem -Path $fileRoot -File | Sort-Object Name)
}

$entrySize = 48
$dataStart = 16 + ($files.Count * $entrySize)
$archiveSize = $dataStart
foreach ($file in $files) { $archiveSize += [int]$file.Length }

if ($archiveSize -gt $archiveRoom) {
  throw "the files add up to $archiveSize bytes and the disk reserves $archiveRoom for them"
}

$archive = New-Object byte[] ([math]::Max($archiveSize, 16))
$magic = [System.Text.Encoding]::ASCII.GetBytes("METTLEFS")
[System.Array]::Copy($magic, 0, $archive, 0, 8)
[System.Array]::Copy([System.BitConverter]::GetBytes([uint32]$files.Count), 0, $archive, 8, 4)

$cursor = $dataStart
for ($i = 0; $i -lt $files.Count; $i++) {
  $file = $files[$i]
  $slot = 16 + ($i * $entrySize)
  $name = [System.Text.Encoding]::ASCII.GetBytes($file.Name)
  if ($name.Length -gt 31) {
    throw "the name $($file.Name) is longer than 31 bytes"
  }
  [System.Array]::Copy($name, 0, $archive, $slot, $name.Length)
  [System.Array]::Copy([System.BitConverter]::GetBytes([uint64]$cursor), 0, $archive, $slot + 32, 8)
  [System.Array]::Copy([System.BitConverter]::GetBytes([uint64]$file.Length), 0, $archive, $slot + 40, 8)
  $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
  [System.Array]::Copy($bytes, 0, $archive, $cursor, $bytes.Length)
  $cursor += $bytes.Length
}
[System.IO.File]::WriteAllBytes($archiveImage, $archive)

function Get-Geometry([int64]$total) {
  if ($total -gt 65535 * 16 * 255) { $total = 65535 * 16 * 255 }
  if ($total -ge 65535 * 16 * 63) {
    $spt = [int64]255
    $heads = [int64]16
    $ch = [int64]($total / $spt)
  } else {
    $spt = [int64]17
    $ch = [int64]($total / $spt)
    $heads = [int64](($ch + 1023) / 1024)
    if ($heads -lt 4) { $heads = [int64]4 }
    if ($ch -ge $heads * 1024 -or $heads -gt 16) {
      $spt = [int64]31
      $heads = [int64]16
      $ch = [int64]($total / $spt)
    }
    if ($ch -ge $heads * 1024) {
      $spt = [int64]63
      $heads = [int64]16
      $ch = [int64]($total / $spt)
    }
  }
  return @([int64]($ch / $heads), $heads, $spt)
}

function Set-BigEndian([byte[]]$target, [int]$offset, [byte[]]$value) {
  $flipped = $value.Clone()
  [System.Array]::Reverse($flipped)
  [System.Array]::Copy($flipped, 0, $target, $offset, $flipped.Length)
}

$disk = $null
if (-not $Fresh -and (Test-Path $Image)) {
  $existing = [System.IO.File]::ReadAllBytes($Image)
  if ($existing.Length -eq $dataBytes + $sectorSize) {
    $disk = New-Object byte[] $dataBytes
    [System.Array]::Copy($existing, 0, $disk, 0, $dataBytes)
    for ($i = 0; $i -lt $systemBytes; $i++) { $disk[$i] = 0 }
  }
}
$carried = $null -ne $disk
if (-not $carried) {
  $disk = New-Object byte[] $dataBytes
}

[System.Array]::Copy($boot, 0, $disk, 0, $boot.Length)

$partitionSectors = $diskSectors - $partitionLba
$slot = 446
$disk[$slot] = 0x80
$disk[$slot + 1] = 0xFE
$disk[$slot + 2] = 0xFF
$disk[$slot + 3] = 0xFF
$disk[$slot + 4] = 0x83
$disk[$slot + 5] = 0xFE
$disk[$slot + 6] = 0xFF
$disk[$slot + 7] = 0xFF
[System.Array]::Copy([System.BitConverter]::GetBytes([uint32]$partitionLba), 0, $disk, $slot + 8, 4)
[System.Array]::Copy([System.BitConverter]::GetBytes([uint32]$partitionSectors), 0, $disk, $slot + 12, 4)
$disk[510] = 0x55
$disk[511] = 0xAA

[System.Array]::Copy($stage, 0, $disk, $stageLba * $sectorSize, $stage.Length)
[System.Array]::Copy($kernel, 0, $disk, $kernelLba * $sectorSize, $kernel.Length)
[System.Array]::Copy($archive, 0, $disk, $archiveLba * $sectorSize, $archive.Length)

$geometry = Get-Geometry $diskSectors
$footer = New-Object byte[] $sectorSize
[System.Array]::Copy([System.Text.Encoding]::ASCII.GetBytes("conectix"), 0, $footer, 0, 8)
Set-BigEndian $footer 8 ([System.BitConverter]::GetBytes([uint32]2))
Set-BigEndian $footer 12 ([System.BitConverter]::GetBytes([uint32]0x00010000))
for ($i = 16; $i -lt 24; $i++) { $footer[$i] = 0xFF }
$epoch = [datetime]::new(2000, 1, 1, 0, 0, 0, [System.DateTimeKind]::Utc)
$stamp = [uint32](([datetime]::UtcNow - $epoch).TotalSeconds)
Set-BigEndian $footer 24 ([System.BitConverter]::GetBytes($stamp))
[System.Array]::Copy([System.Text.Encoding]::ASCII.GetBytes("mtle"), 0, $footer, 28, 4)
Set-BigEndian $footer 32 ([System.BitConverter]::GetBytes([uint32]0x00010000))
[System.Array]::Copy([System.Text.Encoding]::ASCII.GetBytes("Wi2k"), 0, $footer, 36, 4)
Set-BigEndian $footer 40 ([System.BitConverter]::GetBytes([uint64]$dataBytes))
Set-BigEndian $footer 48 ([System.BitConverter]::GetBytes([uint64]$dataBytes))
Set-BigEndian $footer 56 ([System.BitConverter]::GetBytes([uint16]$geometry[0]))
$footer[58] = [byte]$geometry[1]
$footer[59] = [byte]$geometry[2]
Set-BigEndian $footer 60 ([System.BitConverter]::GetBytes([uint32]2))
$identity = [guid]::Parse("6d657474-6c65-6f73-0000-000000000001").ToByteArray()
[System.Array]::Copy($identity, 0, $footer, 68, 16)
$sum = [uint32]0
foreach ($byte in $footer) { $sum = $sum + $byte }
Set-BigEndian $footer 64 ([System.BitConverter]::GetBytes([uint32](-bnot $sum)))

$stream = [System.IO.File]::Open($Image, [System.IO.FileMode]::Create)
$stream.Write($disk, 0, $disk.Length)
$stream.Write($footer, 0, $footer.Length)
$stream.Close()

$partitionMb = [int]($partitionSectors * $sectorSize / 1048576)
Write-Host "boot sector  $bootUsed bytes of code, 446 before the partition table"
Write-Host "second stage $($stage.Length) bytes of $stageRoom reserved"
Write-Host "kernel       $($kernel.Length) bytes of $kernelRoom reserved"
Write-Host "files        $($files.Count) in $($archive.Length) bytes of $archiveRoom reserved"
if ($carried) {
  Write-Host "partition    $partitionMb megabytes, contents carried over"
} else {
  Write-Host "partition    $partitionMb megabytes, blank"
}
Write-Host "image        $Image"
