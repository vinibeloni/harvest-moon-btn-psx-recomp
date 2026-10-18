#requires -version 5.1
<#
Package the Windows build so it runs on another PC, with no compiler and no
repository. Run it from the repository root after sh build.sh and
sh tools/compile_static_overlays.sh.

  .\tools\make_portable.ps1                          # game without disc or save
  .\tools\make_portable.ps1 -IncludeDisc -IncludeSave -Zip

The output folder holds the executable, game.toml, the BIOS, the overlay cache,
the bundled mod catalog, your controls (input.ini, keybinds.ini) and the Visual
C++ runtime DLLs next to the executable, so the other PC needs no installer.

THE DISC IS COPYRIGHTED. It is copied only with -IncludeDisc, which is for moving
YOUR OWN copy between YOUR OWN computers. Never upload or share the result. Without the switch the other PC needs your disc
(.cue + .bin) copied next to the executable.
#>
param(
    [string]$Out = 'dist\HarvestMoonBTN-portable',
    [switch]$IncludeDisc,
    [switch]$IncludeSave,
    [switch]$IncludeMods,
    [switch]$Zip
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$exe = Get-ChildItem build -Filter 'Harvest_Moon*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $exe) { throw 'No build\Harvest_Moon*.exe. Run sh build.sh first.' }
if (-not (Test-Path 'build\cache\SLUS-01115')) {
    Write-Warning 'build\cache\SLUS-01115 is missing: the game will run its overlays in the interpreter (correct, much slower). Run sh tools/compile_static_overlays.sh.'
}

if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null

function Copy-IfExists($from, $to) {
    if (Test-Path -LiteralPath $from) { Copy-Item -Recurse -Force -LiteralPath $from -Destination $to; return $true }
    return $false
}

Copy-Item $exe.FullName $Out
Copy-Item game.toml $Out
foreach ($f in 'input.ini', 'keybinds.ini') { [void](Copy-IfExists "build\$f" $Out) }
[void](Copy-IfExists 'build\bios' "$Out\bios")
[void](Copy-IfExists 'build\cache' "$Out\cache")

# The runtime creates and reads mods\ beside the executable. The bundled catalog is
# framework output; user mods are copied only with -IncludeMods.
New-Item -ItemType Directory -Force "$Out\mods" | Out-Null
[void](Copy-IfExists 'build\mods\bundled' "$Out\mods\bundled")
if ($IncludeMods) { [void](Copy-IfExists 'build\mods\hmbtn' "$Out\mods\hmbtn") }

# Visual C++ runtime, app-local, so the other PC does not need the redistributable.
$rt = 'MSVCP140.dll', 'VCRUNTIME140.dll', 'VCRUNTIME140_1.dll'
foreach ($d in $rt) {
    $src = Join-Path $env:WINDIR "System32\$d"
    if (Test-Path $src) { Copy-Item $src $Out } else { Write-Warning "$d not found in System32; install the VC++ 2015-2022 x64 redistributable on the other PC." }
}

if ($IncludeDisc) {
    # Only the disc: the .cue and the .bin it names. The runtime reads the game
    # executable from the disc, and SLUS_011.15 / *.text.bin are Ghidra leftovers.
    Get-ChildItem -LiteralPath $root -File | Where-Object { $_.Extension -in '.cue', '.bin' -and $_.Name -notlike '*.text.bin' } |
        ForEach-Object { Copy-Item $_.FullName $Out }
}
if ($IncludeSave) { foreach ($f in 'card1.mcd', 'card2.mcd') { [void](Copy-IfExists "build\$f" $Out) } }

@"
Harvest Moon: Back to Nature (recompiled) - portable folder

Run:  $($exe.Name)   (double-click, or Play.bat)

Needs:
  - 64-bit Windows 10 or newer, a GPU with OpenGL.
  - Your disc: HARVEST MOON - BACK TO NATURE (USA).cue + .bin next to the executable
    (already here if this folder was made with -IncludeDisc).
  - Nothing else to install: the Visual C++ runtime is in this folder.

Saves: card1.mcd next to the executable is the memory card. Copy it to keep your saves.
Mods: put mod folders in mods\hmbtn\<name>\mod.toml and restart the game.
Controls: input.ini and keybinds.ini. Ctrl+Y shows the frame-stats overlay.
Save state: Shift+F1..F12 saves, F1..F12 loads.

This folder may contain your copy of the game. Do not upload or share it.
"@ | Set-Content -Encoding utf8 "$Out\README.txt"
"@echo off`r`ncd /d `"%~dp0`"`r`nstart `"`" `"$($exe.Name)`"`r`n" | Set-Content -Encoding ascii "$Out\Play.bat"

$size = (Get-ChildItem $Out -Recurse -File | Measure-Object Length -Sum).Sum / 1MB
Write-Host ('Portable folder: {0}  ({1:N0} MB)' -f (Resolve-Path $Out), $size)
Get-ChildItem $Out | ForEach-Object { '  ' + $_.Name }

if ($Zip) {
    $z = "$Out.zip"
    if (Test-Path $z) { Remove-Item $z }
    & (Get-Command python -ErrorAction SilentlyContinue).Source -c "import shutil,sys; shutil.make_archive(sys.argv[1], 'zip', sys.argv[2])" ($z -replace '\.zip$','') $Out
    Write-Host ('Zip: {0}  ({1:N0} MB)' -f (Resolve-Path $z), ((Get-Item $z).Length / 1MB))
}
if ($IncludeDisc) { Write-Warning 'The output contains your copy of the game. Do not share it.' }
