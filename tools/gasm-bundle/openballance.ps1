# OpenBallance @VERSION@ (openballance.wasm) on the bundled gasm-run @GASM_VERSION@: Windows launcher, started by
# OpenBallance.cmd.
#   OpenBallance.cmd [options] [DATA] [gasm-run options...]          (OpenBallance.cmd --help)
# The game data location comes from the argument (then saved), else %APPDATA%\OpenBallance\data-location, else
# a folder or file picker. Test hooks (no dialogs): OPENBALLANCE_DATA=<folder|image> uses that data without
# saving it, OPENBALLANCE_DRY_RUN=1 or --dry-run prints the gasm-run command instead of running it.
$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$ConfDir = Join-Path $env:APPDATA 'OpenBallance'
$LocFile = Join-Path $ConfDir 'data-location'
$Title = 'Ballance (gasm)'
$Dry = $env:OPENBALLANCE_DRY_RUN -eq '1'
$Change = $false

function Show-Usage {
  @"
OpenBallance @VERSION@ on gasm-run @GASM_VERSION@

  OpenBallance.cmd [options] [DATA] [gasm-run options...]

DATA is your copy of Ballance: the CD's drive (e.g. D:\) or a mounted disc image (it has Setup\data1.hdr),
a folder you copied the CD to, the installed game's folder (it has base.cmo, e.g.
C:\Program Files (x86)\Ballance), or a disc image file (.iso, or the .bin of a .bin/.cue pair). It is saved in
  $LocFile
so later runs need no argument. Without one, a dialog asks for it.

Options:
  --change-data  ask for the game data even if a location is saved
  --forget-data  delete the saved location and exit
  --dry-run      print the gasm-run command instead of running it (also OPENBALLANCE_DRY_RUN=1)
  --help         this text
Anything after DATA goes to gasm-run, e.g. --param unlockall=1, --param language=0, --keymap FILE, --mute.
OPENBALLANCE_DATA=<DATA> uses that data for one run without saving it.

More: https://openballance.emdzej.pl/guide/
"@
}

# $null if usable, else the reason (Windows paths are case-insensitive)
function Test-Data([string]$p) {
  if (Test-Path -LiteralPath $p -PathType Container) {
    if (Test-Path -LiteralPath (Join-Path $p 'base.cmo') -PathType Leaf) { return $null }
    if (Test-Path -LiteralPath (Join-Path $p 'Setup\data1.hdr') -PathType Leaf) { return $null }
    return "This folder is neither the Ballance CD (it has Setup\data1.hdr) nor the installed game (it has base.cmo):`n$p"
  }
  if (Test-Path -LiteralPath $p -PathType Leaf) {
    switch ([IO.Path]::GetExtension($p).ToLowerInvariant()) {
      '.iso' { return $null }
      '.bin' { return $null }
      '.cue' { return 'This is the cue sheet: choose the .bin file next to it.' }
      default { return "Not a disc image: use an .iso/.bin disc image or the CD/installed folder.`n$p" }
    }
  }
  return "The Ballance game data was not found at:`n$p`nInsert or mount the CD, or choose it again."
}

function Get-Absolute([string]$p) {
  $full = [IO.Path]::GetFullPath([IO.Path]::Combine((Get-Location).ProviderPath, $p))
  if ($full.Length -gt 3) { $full = $full.TrimEnd('\') }   # keep D:\ as it is
  return $full
}

# A small dialog: folder / disc image / Quit; then the picker. Returns the path or $null.
function Request-Data([string]$message) {
  Add-Type -AssemblyName System.Windows.Forms
  [System.Windows.Forms.Application]::EnableVisualStyles()
  $form = New-Object System.Windows.Forms.Form
  $form.Text = $Title; $form.FormBorderStyle = 'FixedDialog'; $form.StartPosition = 'CenterScreen'
  $form.MaximizeBox = $false; $form.MinimizeBox = $false; $form.AutoSize = $true; $form.AutoSizeMode = 'GrowAndShrink'
  $form.Padding = New-Object System.Windows.Forms.Padding(12)
  $label = New-Object System.Windows.Forms.Label
  $label.Text = $message; $label.AutoSize = $true; $label.MaximumSize = New-Object System.Drawing.Size(500, 0)
  $label.Location = New-Object System.Drawing.Point(12, 12)
  $form.Controls.Add($label)
  $y = $label.PreferredHeight + 28
  $buttons = @(@('Choose your Ballance folder...', 'folder'), @('Choose disc image (.iso, .bin)...', 'image'), @('Quit', 'quit'))
  $x = 12
  foreach ($b in $buttons) {
    $btn = New-Object System.Windows.Forms.Button
    $btn.Text = $b[0]; $btn.Tag = $b[1]; $btn.AutoSize = $true; $btn.Location = New-Object System.Drawing.Point($x, $y)
    $btn.Add_Click({ $form.Tag = $this.Tag; $form.Close() })
    $form.Controls.Add($btn); $x += 190
  }
  $form.AcceptButton = $form.Controls[1]; $form.CancelButton = $form.Controls[3]
  [void]$form.ShowDialog()
  switch ($form.Tag) {
    'folder' {
      $d = New-Object System.Windows.Forms.FolderBrowserDialog
      $d.Description = 'Choose the Ballance CD (the drive or folder with Setup) or the installed game (the folder with base.cmo)'
      $d.ShowNewFolderButton = $false
      if ($d.ShowDialog() -eq 'OK') { return $d.SelectedPath }
    }
    'image' {
      $d = New-Object System.Windows.Forms.OpenFileDialog
      $d.Title = 'Choose the Ballance CD image (.iso, or the .bin of a .bin/.cue pair)'
      $d.Filter = 'Disc images (*.iso;*.bin)|*.iso;*.bin|All files (*.*)|*.*'
      if ($d.ShowDialog() -eq 'OK') { return $d.FileName }
    }
  }
  return $null
}

$Intro = @"
OpenBallance needs your copy of Ballance (it is not included).

Choose the CD itself or a mounted disc image (right-click an .iso, Mount: it gets a drive letter), a folder you copied the CD to, the folder of an installed Ballance (with base.cmo), or an .iso or .bin image file.

Your choice is remembered; run OpenBallance.cmd --change-data to pick another.
"@

$rest = @($args)
while ($rest.Count -gt 0) {
  $a = [string]$rest[0]
  if ($a -eq '--help' -or $a -eq '-h' -or $a -eq '/?') { Show-Usage; exit 0 }
  elseif ($a -eq '--dry-run') { $Dry = $true }
  elseif ($a -eq '--change-data' -or $a -eq '--change-cd') { $Change = $true }
  elseif ($a -eq '--forget-data' -or $a -eq '--forget-cd') {
    Remove-Item -LiteralPath $LocFile -ErrorAction SilentlyContinue; "forgot the game data location ($LocFile)"; exit 0
  }
  else { break }
  $rest = @($rest | Select-Object -Skip 1)
}
$data = $null; $save = $false
if ($rest.Count -gt 0 -and -not ([string]$rest[0]).StartsWith('-')) {
  $data = Get-Absolute ([string]$rest[0]); $save = $true; $rest = @($rest | Select-Object -Skip 1)
}
if (-not $data -and $env:OPENBALLANCE_DATA) { $data = Get-Absolute $env:OPENBALLANCE_DATA }
if (-not $data -and -not $Change -and (Test-Path -LiteralPath $LocFile)) {
  $data = (Get-Content -LiteralPath $LocFile -TotalCount 1).Trim()
}

$msg = $Intro
if ($data) { $msg = Test-Data $data }
while (-not $data -or (Test-Data $data)) {
  if ($Dry -or $env:OPENBALLANCE_DATA) {   # never open dialogs in test runs
    if ($data) { [Console]::Error.WriteLine("no usable Ballance game data: $msg") } else { [Console]::Error.WriteLine('no usable Ballance game data') }
    exit 2
  }
  $data = Request-Data $msg
  if (-not $data) { exit 0 }
  $data = Get-Absolute $data; $save = $true
  $msg = Test-Data $data
}
if ($save -and -not $Dry) {
  New-Item -ItemType Directory -Force -Path $ConfDir | Out-Null
  Set-Content -LiteralPath $LocFile -Value $data -Encoding UTF8
}

if (Test-Path -LiteralPath $data -PathType Container) { $src = @('--asset-dir', $data) } else { $src = @('--rom', $data) }
$run = Join-Path $Here 'gasm-run.exe'
# OpenBallance's keyboard layout (Shift + arrows rotate the view), unless the user has their own gasm layout
$km = @()
if (($rest -notcontains '--keymap') -and -not (Test-Path -LiteralPath (Join-Path $env:APPDATA 'gasm\keymap.txt'))) {
  $km = @('--keymap', (Join-Path $Here 'keymap.txt'))
}
$cmd = @((Join-Path $Here 'openballance.wasm')) + $src + @('--window', '1280x960') + $km + $rest
if ($Dry) { (@($run) + $cmd | ForEach-Object { '"' + $_ + '"' }) -join ' '; exit 0 }
& $run @cmd
exit $LASTEXITCODE
