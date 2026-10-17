param([string]$Table = (Join-Path $PSScriptRoot "strings.tsv"), [string]$StringsDir = $PSScriptRoot)
# Generates <lang>\Resources.resw from strings.tsv (key<TAB>English<TAB>Greek).
# To change or translate text: edit strings.tsv, then run .\make-resw.ps1
$ErrorActionPreference = 'Stop'
$header = @'
<?xml version="1.0" encoding="utf-8"?>
<root>
  <resheader name="resmimetype"><value>text/microsoft-resx</value></resheader>
  <resheader name="version"><value>2.0</value></resheader>
  <resheader name="reader"><value>System.Resources.ResXResourceReader, System.Windows.Forms, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089</value></resheader>
  <resheader name="writer"><value>System.Resources.ResXResourceWriter, System.Windows.Forms, Version=4.0.0.0, Culture=neutral, PublicKeyToken=b77a5c561934e089</value></resheader>
'@
$rows = Get-Content -LiteralPath $Table -Encoding UTF8 | Where-Object { $_.Trim() -ne '' } | ForEach-Object { , ($_ -split "`t") }
$seen = @{}
foreach ($row in $rows) {
    if ($row.Count -ne 3) { throw "Bad row (needs 3 columns): $($row -join ' | ')" }
    if ($seen.ContainsKey($row[0])) { throw "Duplicate key: $($row[0])" }
    $seen[$row[0]] = $true
}
foreach ($lang in @(@{ Tag = 'en-US'; Col = 1 }, @{ Tag = 'el-GR'; Col = 2 })) {
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
