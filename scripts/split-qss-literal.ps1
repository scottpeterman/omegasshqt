<#
  Re-splits the QSS raw literal in theme\tokenstylesheet.cpp so no single string
  literal exceeds MSVC's 16380-byte cap (C2026). Idempotent: collapses any
  existing split first, then re-packs. Adjacent literals are concatenated by the
  compiler, so the resulting string is byte-for-byte identical.
#>
param(
    [string]$Path = "theme\tokenstylesheet.cpp",
    [int]$Cap = 15000   # headroom under MSVC's 16380
)

$ErrorActionPreference = "Stop"

function ByteLen([string]$s) { [System.Text.Encoding]::UTF8.GetByteCount($s) }

function Split-ByLines([string]$chunk, [int]$cap) {
    $out = New-Object System.Collections.Generic.List[string]
    $acc = ""
    foreach ($line in [regex]::Split($chunk, '(?<=\n)')) {
        if ($line -eq "") { continue }
        if ($acc -ne "" -and (ByteLen ($acc + $line)) -gt $cap) { $out.Add($acc); $acc = "" }
        $acc += $line
    }
    if ($acc -ne "") { $out.Add($acc) }
    $out
}

$full  = (Resolve-Path $Path).Path
$bytes = [System.IO.File]::ReadAllBytes($full)
$hasBom = ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)
$off = if ($hasBom) { 3 } else { 0 }
$raw = [System.Text.Encoding]::UTF8.GetString($bytes, $off, $bytes.Length - $off)

$eol  = if ($raw.Contains("`r`n")) { "`r`n" } else { "`n" }
$join = ')QSS"' + $eol + '    R"QSS('

$startTok = 'R"QSS('
$endTok   = ')QSS"'
$start = $raw.IndexOf($startTok)
$end   = $raw.LastIndexOf($endTok)
if ($start -lt 0 -or $end -lt 0) { throw "kTemplate literal not found in $full" }
$start += $startTok.Length

# Collapse any prior split -- the seam whitespace is artifact, not content.
$body = $raw.Substring($start, $end - $start)
$body = [regex]::Replace($body, '\)QSS"\r?\n\s*R"QSS\(', '')
$original = $body

# Prefer breaking at the existing section comments.
$marker = '/* ---- '
$cuts = New-Object System.Collections.Generic.List[int]
$cuts.Add(0)
foreach ($m in [regex]::Matches($body, [regex]::Escape($marker))) {
    if ($m.Index -gt 0) { $cuts.Add($m.Index) }
}
$cuts.Add($body.Length)

$sections = New-Object System.Collections.Generic.List[string]
for ($i = 0; $i -lt $cuts.Count - 1; $i++) {
    $sections.Add($body.Substring($cuts[$i], $cuts[$i + 1] - $cuts[$i]))
}

# Greedily pack sections into literals under the cap.
$packed = New-Object System.Collections.Generic.List[string]
$cur = ""
foreach ($sec in $sections) {
    if ($cur -ne "" -and (ByteLen ($cur + $sec)) -gt $Cap) { $packed.Add($cur); $cur = $sec }
    else { $cur += $sec }
}
if ($cur -ne "") { $packed.Add($cur) }

# Any single section still over the cap gets split on line boundaries.
$pieces = New-Object System.Collections.Generic.List[string]
foreach ($p in $packed) {
    if ((ByteLen $p) -gt $Cap) { foreach ($q in (Split-ByLines $p $Cap)) { $pieces.Add($q) } }
    else { $pieces.Add($p) }
}

if ([string]::Join('', $pieces) -ne $original) { throw "re-split changed the string content" }
foreach ($p in $pieces) {
    if ((ByteLen $p) -gt 16380) { throw "piece still over MSVC's cap" }
}

$out = $raw.Substring(0, $start) + [string]::Join($join, $pieces) + $raw.Substring($end)
[System.IO.File]::WriteAllText($full, $out, (New-Object System.Text.UTF8Encoding($hasBom)))

$max = ($pieces | ForEach-Object { ByteLen $_ } | Measure-Object -Maximum).Maximum
"kTemplate: $(ByteLen $original) bytes -> $($pieces.Count) literals (max $max bytes each)"