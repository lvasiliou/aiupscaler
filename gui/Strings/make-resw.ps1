param([string]$Table = (Join-Path $PSScriptRoot "strings.tsv"), [string]$StringsDir = $PSScriptRoot)
# Generates <lang>\Resources.resw from strings.tsv: key, then one column per language in the
# order of $languages below. To change or translate text: edit strings.tsv, then run .\make-resw.ps1
$ErrorActionPreference = 'Stop'
$header = @'
<?xml version="1.0" encoding="utf-8"?>
<root>
  <resheader name="resmimetype"><value>text/microsoft-resx</value></resheader>
  <resheader name="version"><value>2.0</value></resheader>
  <resheader name="reader"><value>System.Resources.ResXResourceReader, System.Windows.Forms, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089</value></resheader>
  <resheader name="writer"><value>System.Resources.ResXResourceWriter, System.Windows.Forms, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089</value></resheader>
'@
$languages = 'en-US', 'el-GR', 'de-DE', 'fr-FR', 'zh-CN', 'tr-TR', 'ja-JP'
$rows = Get-Content -LiteralPath $Table -Encoding UTF8 | Where-Object { $_.Trim() -ne '' } | ForEach-Object { , ($_ -split "`t") }
$seen = @{}
foreach ($row in $rows) {
    if ($row.Count -ne $languages.Count + 1) { throw "Bad row (needs $($languages.Count + 1) columns): $($row -join ' | ')" }
    # Code fills {0}, {1}...: every translation must keep the English placeholders.
    $placeholders = [regex]::Matches($row[1], '\{\d+\}') | ForEach-Object Value | Sort-Object -Unique
    for ($col = 2; $col -lt $row.Count; $col++) {
        $found = [regex]::Matches($row[$col], '\{\d+\}') | ForEach-Object Value | Sort-Object -Unique
        if ((@($placeholders) -join ',') -ne (@($found) -join ',')) { throw "Placeholders differ in $($languages[$col - 1]) for $($row[0])" }
    }
    if ($seen.ContainsKey($row[0])) { throw "Duplicate key: $($row[0])" }
    $seen[$row[0]] = $true
}
foreach ($lang in (0..($languages.Count - 1) | ForEach-Object { @{ Tag = $languages[$_]; Col = $_ + 1 } })) {
    $sb = [System.Text.StringBuilder]::new($header)
    foreach ($row in $rows) {
        $name = [System.Security.SecurityElement]::Escape($row[0])
        $value = [System.Security.SecurityElement]::Escape($row[$lang.Col])
        [void]$sb.AppendLine("  <data name=`"$name`" xml:space=`"preserve`"><value>$value</value></data>")
    }
    [void]$sb.AppendLine('</root>')
    $dir = Join-Path $StringsDir $lang.Tag
    New-Item -ItemType Directory -Force $dir | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $dir 'Resources.resw'), $sb.ToString(), [System.Text.UTF8Encoding]::new($false))
    [xml](Get-Content -LiteralPath (Join-Path $dir 'Resources.resw') -Raw -Encoding UTF8) | Out-Null  # validates the XML
    "$($lang.Tag): $($rows.Count) strings"
}
