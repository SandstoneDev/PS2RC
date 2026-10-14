# card_sync.ps1 - copy a folder tree to the PSP card file by file, verifying every file's MD5.
# A PSP's USB link can drop in the middle of writes: a file is copied only when the card's copy is missing or
# differs, then re-hashed; up to 3 attempts; the result is one line per file.
#   powershell -ExecutionPolicy Bypass -File tools\dev\card_sync.ps1 -Src <dir> -Dst <dir> [-Log <file>]
param([Parameter(Mandatory = $true)][string]$Src, [Parameter(Mandatory = $true)][string]$Dst, [string]$Log = "")
$ErrorActionPreference = "Continue"
# (a relative -Src would cut the wrong number of characters off FullName and nest the tree under Dst)
$Src = (Resolve-Path -LiteralPath $Src).ProviderPath
$files = Get-ChildItem -LiteralPath $Src -Recurse | Where-Object { -not $_.PSIsContainer }
$ok = 0; $copied = 0; $fail = 0
foreach ($f in $files) {
    $rel = $f.FullName.Substring($Src.TrimEnd('\').Length).TrimStart('\')
    $d = Join-Path $Dst $rel
    $want = (Get-FileHash -LiteralPath $f.FullName -Algorithm MD5).Hash
    $good = $false
    for ($i = 0; $i -lt 3 -and -not $good; $i++) {
        try {
            if (-not (Test-Path -LiteralPath $d) -or (Get-FileHash -LiteralPath $d -Algorithm MD5).Hash -ne $want) {
                New-Item -ItemType Directory -Force -Path (Split-Path $d) | Out-Null
                Copy-Item -LiteralPath $f.FullName -Destination $d -Force
                $copied++
            }
            $good = (Get-FileHash -LiteralPath $d -Algorithm MD5).Hash -eq $want
        } catch {
            Start-Sleep -Seconds 2
        }
    }
    $line = "{0} {1} {2,12} {3}" -f $(if ($good) { "OK  " } else { "FAIL" }), $want.Substring(0, 8), $f.Length, $rel
    if ($good) { $ok++ } else { $fail++ }
    if ($Log) { Add-Content -LiteralPath $Log -Value $line } else { $line }
}
$sum = "card_sync: {0} files OK, {1} copies made, {2} FAILED ({3} -> {4})" -f $ok, $copied, $fail, $Src, $Dst
if ($Log) { Add-Content -LiteralPath $Log -Value $sum }
$sum
