param(
    [string]$Root = "."
)

$targets = @(
    "slint_ui/src",
    "slint_ui/ui",
    "main/ui"
)

$extensions = @(".rs", ".slint", ".cpp", ".h")

# Unicode-safe mojibake detector:
# - \u0420[\u0450-\u045F] and \u0421[\u0450-\u045F] catch patterns like "Рџ", "СЏ"
# - \u0432\u0402 catches "вЂ"
# - \u00D0 / \u00D1 followed by ASCII catches "Ð..." / "Ñ..."
$badRegex = [regex]'(?:\u0420[\u0450-\u045F]|\u0421[\u0450-\u045F]|\u0432\u0402|\u00D0[A-Za-z]|\u00D1[A-Za-z])'

$rootPath = Resolve-Path $Root
$issues = @()

foreach ($target in $targets) {
    $dir = Join-Path $rootPath $target
    if (-not (Test-Path $dir)) { continue }

    Get-ChildItem -Path $dir -Recurse -File |
        Where-Object { $extensions -contains $_.Extension } |
        ForEach-Object {
            $file = $_.FullName
            $text = [System.IO.File]::ReadAllText($file, [System.Text.Encoding]::UTF8)
            $lines = $text -split "`r?`n"
            for ($i = 0; $i -lt $lines.Length; $i++) {
                if ($badRegex.IsMatch($lines[$i])) {
                    $issues += "{0}:{1}: {2}" -f $file, ($i + 1), $lines[$i].Trim()
                }
            }
        }
}

if ($issues.Count -gt 0) {
    Write-Host "Mojibake check failed. Found suspicious fragments:" -ForegroundColor Red
    $issues | ForEach-Object { Write-Host $_ -ForegroundColor Yellow }
    exit 1
}

Write-Host "Mojibake check passed." -ForegroundColor Green
exit 0

