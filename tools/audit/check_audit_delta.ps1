# AUDIT_DELTA parity and completeness check (EN/DE/RU)
#
# 1. The three language versions must contain the same card IDs.
# 2. Every card must carry the full field set (Diff included) in every language.
# 3. Reports how many TODO placeholders are still open.
#
# Non-zero exit means the audit delta is not ready for handover.
param(
    [string]$Dir = (Join-Path $PSScriptRoot '..\..\docs\modernization')
)

$files = [ordered]@{
    en = Join-Path $Dir 'AUDIT_DELTA.en.md'
    de = Join-Path $Dir 'AUDIT_DELTA.de.md'
    ru = Join-Path $Dir 'AUDIT_DELTA.ru.md'
}

# Field labels as they appear at the start of a card line ("- <label>:").
# Diff is optional: it is required only on the key cards that must show a real
# diff fragment (reported separately, at least 8 expected).
$labels = [ordered]@{
    en = @('Status', 'Commits', 'Files', 'Rust reference', 'Before', 'After', 'Evidence', 'Risk, rollback')
    de = @('Status', 'Commits', 'Dateien', 'Rust-Referenz', 'Vorher', 'Nachher', 'Nachweis', 'Risiko, Rücknahme')
    ru = @('Статус', 'Коммиты', 'Файлы', 'Ссылка на Rust', 'Было', 'Стало', 'Доказательство', 'Риск, откат')
}

$diffLabels = [ordered]@{ en = 'Diff'; de = 'Diff'; ru = 'Дифф' }

$cards = @{}
$diffCards = @{}
$ok = $true

foreach ($lang in $files.Keys) {
    $path = $files[$lang]
    if (-not (Test-Path -LiteralPath $path)) {
        Write-Output "MISSING FILE: $path"
        exit 2
    }
    $cards[$lang] = @(
        Select-String -Path $path -Pattern '^### (G-[0-9]{2}[a-z]?(?: / G-[0-9]{2}[a-z]?)*)' |
            ForEach-Object { $_.Matches[0].Groups[1].Value }
    )

    # Cards that actually carry a diff block, in document order and unique.
    $diffCards[$lang] = @()
    $current = $null
    foreach ($line in Get-Content $path) {
        if ($line -match '^### (G-[0-9]{2}[a-z]?(?: / G-[0-9]{2}[a-z]?)*)') {
            $current = $Matches[1]
        }
        elseif ($current -and $line -match ('^- ' + [regex]::Escape($diffLabels[$lang]) + ':')) {
            if ($diffCards[$lang] -notcontains $current) { $diffCards[$lang] += $current }
        }
    }
}

$all = @($cards.Values | ForEach-Object { $_ } | Select-Object -Unique)

foreach ($id in $all) {
    foreach ($lang in $files.Keys) {
        if ($cards[$lang] -notcontains $id) {
            Write-Output "MISSING CARD in ${lang}: $id"
            $ok = $false
        }
    }
}

foreach ($lang in $files.Keys) {
    $path = $files[$lang]
    $count = $cards[$lang].Count
    foreach ($label in $labels[$lang]) {
        $found = (Select-String -Path $path -Pattern ('^- ' + [regex]::Escape($label) + ':')).Count
        if ($found -ne $count) {
            Write-Output "FIELD MISMATCH in ${lang}: '$label' found $found of $count"
            $ok = $false
        }
    }
    $todo = (Select-String -Path $path -Pattern 'TODO|TBD' -AllMatches | Measure-Object).Count
    $diffs = (Select-String -Path $path -Pattern ('^- ' + [regex]::Escape($diffLabels[$lang]) + ':')).Count
    if ($diffs -lt 8) {
        Write-Output "TOO FEW DIFF BLOCKS in ${lang}: $diffs (expected at least 8 key cards)"
        $ok = $false
    }
    Write-Output ("{0}: {1} cards, {2} mandatory fields checked, {3} diff blocks, {4} lines with TODO" -f $lang, $count, $labels[$lang].Count, $diffs, $todo)
}

# Diff blocks must cover exactly the same cards in all three languages.
$reference = $diffCards['en']
foreach ($lang in @('de', 'ru')) {
    $missing = @($reference | Where-Object { $diffCards[$lang] -notcontains $_ })
    $extra = @($diffCards[$lang] | Where-Object { $reference -notcontains $_ })
    if ($missing.Count -gt 0) {
        Write-Output "MISSING DIFF in ${lang}: $($missing -join ', ')"
        $ok = $false
    }
    if ($extra.Count -gt 0) {
        Write-Output "EXTRA DIFF in ${lang}: $($extra -join ', ')"
        $ok = $false
    }
}

if (-not $ok) {
    Write-Output 'PARITY FAILED'
    exit 1
}

Write-Output ("PARITY OK: {0} card IDs, {1} mandatory fields per card, {2} diff cards in 3 languages" -f $all.Count, $labels['en'].Count, $reference.Count)
