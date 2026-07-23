[CmdletBinding()]
param(
    [string]$Config,
    [switch]$SelfTest
)

$ErrorActionPreference = 'Stop'
$script:IsWindowsPlatform =
    [Environment]::OSVersion.Platform -eq [PlatformID]::Win32NT
$script:RepositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$script:ProbeProfile = 'deterministic-v1'
$script:ProbeMarker = 'KATANA_RUNTIME_PROBE '
$script:FaultMarker = 'KATANA_RUNTIME_PROBE_FAULT '
$script:CheckpointMarker = 'KATANA_RUNTIME_PROBE_CHECKPOINT '
$script:TraceMarker = 'KATANA_WAIT_LOOP_TRACE '
$script:TraceNotice =
    'KATANA_WAIT_LOOP_TRACE_NOTICE local-only; contains raw guest-memory values; do not share without review'
$script:MaximumCaptureCharacters = 16 * 1024 * 1024

function Throw-ProbeFailure {
    param([Parameter(Mandatory = $true)][string]$Class)
    $exception = [InvalidOperationException]::new('katana-private-runtime-probe-failed')
    $exception.Data['failure_class'] = $Class
    throw $exception
}

function Get-ProbeFailureClass {
    param($Exception)
    if ($null -ne $Exception -and
        $null -ne $Exception.Data -and
        $Exception.Data.Contains('failure_class')) {
        return [string]$Exception.Data['failure_class']
    }
    return 'internal-error'
}

function Test-IntegralJsonNumber {
    param($Value)
    if ($null -eq $Value) { return $false }
    $typeCode = [Type]::GetTypeCode($Value.GetType())
    return $typeCode -in @(
        [TypeCode]::SByte,
        [TypeCode]::Byte,
        [TypeCode]::Int16,
        [TypeCode]::UInt16,
        [TypeCode]::Int32,
        [TypeCode]::UInt32,
        [TypeCode]::Int64,
        [TypeCode]::UInt64
    )
}

function Test-ZeroScaleDecimal {
    param($Value)
    if ($Value -isnot [decimal]) { return $false }
    $flags = [decimal]::GetBits([decimal]$Value)[3]
    return (($flags -shr 16) -band 0x7f) -eq 0
}

function Get-StrictHostTimeoutSeconds {
    param($Value)
    if (-not (Test-IntegralJsonNumber $Value) -or
        [decimal]$Value -lt 1 -or [decimal]$Value -gt 900) {
        Throw-ProbeFailure 'invalid-host-timeout'
    }
    return [int]$Value
}

function Get-StrictUInt64 {
    param(
        $Value,
        [switch]$Positive
    )
    $integral = Test-IntegralJsonNumber $Value
    if (-not $integral -and -not (Test-ZeroScaleDecimal $Value)) {
        Throw-ProbeFailure 'invalid-unsigned-integer'
    }
    try {
        $decimalValue = [decimal]$Value
        if ($decimalValue -lt 0 -or
            $decimalValue -gt [decimal][uint64]::MaxValue -or
            ($Positive -and $decimalValue -eq 0)) {
            Throw-ProbeFailure 'invalid-unsigned-integer'
        }
        return [uint64]$decimalValue
    } catch {
        if ($_.Exception.Data.Contains('failure_class')) { throw }
        Throw-ProbeFailure 'invalid-unsigned-integer'
    }
}

function Test-JsonBoolean {
    param($Value)
    return $null -ne $Value -and $Value.GetType() -eq [bool]
}

function Assert-ExactFields {
    param(
        $Object,
        [string[]]$Required,
        [string[]]$Optional = @(),
        [string]$FailureClass = 'invalid-json-contract'
    )
    if ($null -eq $Object -or $Object -isnot [pscustomobject]) {
        Throw-ProbeFailure $FailureClass
    }
    $actual = @($Object.PSObject.Properties.Name)
    $allowed = @($Required + $Optional)
    $unknown = @($actual | Where-Object { $allowed -cnotcontains $_ })
    $missing = @($Required | Where-Object { $actual -cnotcontains $_ })
    if ($unknown.Count -ne 0 -or $missing.Count -ne 0 -or
        $actual.Count -ne ($Required.Count + @(
            $Optional | Where-Object { $actual -ccontains $_ }).Count)) {
        Throw-ProbeFailure $FailureClass
    }
}

function Initialize-JsonTokenSupport {
    if ('KatanaProbeJson.DuplicateGuard' -as [type]) { return }
    try {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;

namespace KatanaProbeJson
{
    public static class DuplicateGuard
    {
        public static void AssertNoDuplicateMembers(string json)
        {
            if (json == null)
                throw new ArgumentNullException("json");
            new Scanner(json).ParseDocument();
        }

        private sealed class Scanner
        {
            private readonly string text;
            private int offset;
            private int depth;

            internal Scanner(string text)
            {
                this.text = text;
            }

            internal void ParseDocument()
            {
                if (text.Length != 0 && text[0] == '\ufeff')
                    offset++;
                SkipWhitespace();
                ParseValue();
                SkipWhitespace();
                if (offset != text.Length)
                    Fail();
            }

            private void ParseValue()
            {
                if (++depth > 128)
                    Fail();
                try
                {
                    SkipWhitespace();
                    if (offset >= text.Length)
                        Fail();
                    switch (text[offset])
                    {
                        case '{':
                            ParseObject();
                            return;
                        case '[':
                            ParseArray();
                            return;
                        case '"':
                            ParseString();
                            return;
                        case 't':
                            ParseLiteral("true");
                            return;
                        case 'f':
                            ParseLiteral("false");
                            return;
                        case 'n':
                            ParseLiteral("null");
                            return;
                        default:
                            ParseNumber();
                            return;
                    }
                }
                finally
                {
                    depth--;
                }
            }

            private void ParseObject()
            {
                Require('{');
                SkipWhitespace();
                HashSet<string> members = new HashSet<string>(StringComparer.Ordinal);
                if (Consume('}'))
                    return;
                for (;;)
                {
                    SkipWhitespace();
                    string member = ParseString();
                    if (!members.Add(member))
                        throw new InvalidDataException("duplicate-json-member");
                    SkipWhitespace();
                    Require(':');
                    ParseValue();
                    SkipWhitespace();
                    if (Consume('}'))
                        return;
                    Require(',');
                }
            }

            private void ParseArray()
            {
                Require('[');
                SkipWhitespace();
                if (Consume(']'))
                    return;
                for (;;)
                {
                    ParseValue();
                    SkipWhitespace();
                    if (Consume(']'))
                        return;
                    Require(',');
                }
            }

            private string ParseString()
            {
                Require('"');
                StringBuilder value = new StringBuilder();
                while (offset < text.Length)
                {
                    char current = text[offset++];
                    if (current == '"')
                        return value.ToString();
                    if (current < 0x20)
                        Fail();
                    if (current != '\\')
                    {
                        value.Append(current);
                        continue;
                    }
                    if (offset >= text.Length)
                        Fail();
                    char escaped = text[offset++];
                    switch (escaped)
                    {
                        case '"':
                        case '\\':
                        case '/':
                            value.Append(escaped);
                            break;
                        case 'b':
                            value.Append('\b');
                            break;
                        case 'f':
                            value.Append('\f');
                            break;
                        case 'n':
                            value.Append('\n');
                            break;
                        case 'r':
                            value.Append('\r');
                            break;
                        case 't':
                            value.Append('\t');
                            break;
                        case 'u':
                            value.Append(ParseUnicodeEscape());
                            break;
                        default:
                            Fail();
                            break;
                    }
                }
                Fail();
                return null;
            }

            private char ParseUnicodeEscape()
            {
                if (offset > text.Length - 4)
                    Fail();
                int value = 0;
                for (int index = 0; index < 4; ++index)
                {
                    char character = text[offset++];
                    int digit;
                    if (character >= '0' && character <= '9')
                        digit = character - '0';
                    else if (character >= 'a' && character <= 'f')
                        digit = character - 'a' + 10;
                    else if (character >= 'A' && character <= 'F')
                        digit = character - 'A' + 10;
                    else
                    {
                        Fail();
                        return '\0';
                    }
                    value = checked(value * 16 + digit);
                }
                return (char)value;
            }

            private void ParseNumber()
            {
                if (Consume('-') && offset >= text.Length)
                    Fail();
                if (Consume('0'))
                {
                    if (offset < text.Length && IsDigit(text[offset]))
                        Fail();
                }
                else
                {
                    if (offset >= text.Length || text[offset] < '1' || text[offset] > '9')
                        Fail();
                    while (offset < text.Length && IsDigit(text[offset]))
                        offset++;
                }
                if (Consume('.'))
                {
                    if (offset >= text.Length || !IsDigit(text[offset]))
                        Fail();
                    while (offset < text.Length && IsDigit(text[offset]))
                        offset++;
                }
                if (offset < text.Length && (text[offset] == 'e' || text[offset] == 'E'))
                {
                    offset++;
                    if (offset < text.Length && (text[offset] == '+' || text[offset] == '-'))
                        offset++;
                    if (offset >= text.Length || !IsDigit(text[offset]))
                        Fail();
                    while (offset < text.Length && IsDigit(text[offset]))
                        offset++;
                }
            }

            private void ParseLiteral(string literal)
            {
                if (offset > text.Length - literal.Length ||
                    string.CompareOrdinal(text, offset, literal, 0, literal.Length) != 0)
                    Fail();
                offset += literal.Length;
            }

            private void SkipWhitespace()
            {
                while (offset < text.Length)
                {
                    char current = text[offset];
                    if (current != ' ' && current != '\t' &&
                        current != '\r' && current != '\n')
                        return;
                    offset++;
                }
            }

            private bool Consume(char expected)
            {
                if (offset >= text.Length || text[offset] != expected)
                    return false;
                offset++;
                return true;
            }

            private void Require(char expected)
            {
                if (!Consume(expected))
                    Fail();
            }

            private static bool IsDigit(char value)
            {
                return value >= '0' && value <= '9';
            }

            private static void Fail()
            {
                throw new InvalidDataException("invalid-json-token-stream");
            }
        }
    }
}
'@ -Language CSharp
    } catch {
        Throw-ProbeFailure 'json-tokenizer-unavailable'
    }
}

function Read-JsonObject {
    param(
        [string]$Text,
        [string]$FailureClass
    )
    try {
        Initialize-JsonTokenSupport
        [KatanaProbeJson.DuplicateGuard]::AssertNoDuplicateMembers($Text)
        $value = $Text | ConvertFrom-Json -ErrorAction Stop
    } catch {
        Throw-ProbeFailure $FailureClass
    }
    if ($null -eq $value -or $value -isnot [pscustomobject]) {
        Throw-ProbeFailure $FailureClass
    }
    return $value
}

function ConvertFrom-ProbeConfig {
    param([string]$Text)
    $settings = Read-JsonObject $Text 'invalid-config'
    $fields = @(
        'port_root',
        'executable_relative',
        'packed_disc_path',
        'output_root',
        'host_timeout_seconds',
        'guest_cycle_budget'
    )
    Assert-ExactFields $settings $fields @() 'invalid-config'
    foreach ($field in @(
        'port_root',
        'executable_relative',
        'packed_disc_path',
        'output_root'
    )) {
        if ($settings.$field -isnot [string] -or
            [string]::IsNullOrWhiteSpace([string]$settings.$field) -or
            ([string]$settings.$field).IndexOf([char]0) -ge 0) {
            Throw-ProbeFailure 'invalid-config'
        }
    }
    $timeout = Get-StrictHostTimeoutSeconds $settings.host_timeout_seconds
    $budget = Get-StrictUInt64 $settings.guest_cycle_budget -Positive
    return [pscustomobject]@{
        port_root = [string]$settings.port_root
        executable_relative = [string]$settings.executable_relative
        packed_disc_path = [string]$settings.packed_disc_path
        output_root = [string]$settings.output_root
        host_timeout_seconds = $timeout
        guest_cycle_budget = $budget
    }
}

function Assert-NoExistingReparseComponents {
    param(
        [string]$Path,
        [string]$FailureClass = 'reparse-component-forbidden'
    )
    try {
        $full = [IO.Path]::GetFullPath($Path)
        $root = [IO.Path]::GetPathRoot($full)
        if ([string]::IsNullOrWhiteSpace($root)) {
            Throw-ProbeFailure $FailureClass
        }
        $current = $root
        $relative = $full.Substring($root.Length)
        $parts = @($relative -split '[\\/]' | Where-Object { $_.Length -ne 0 })
        if (Test-Path -LiteralPath $current) {
            $rootAttributes = [IO.File]::GetAttributes($current)
            if (($rootAttributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Throw-ProbeFailure $FailureClass
            }
        }
        foreach ($part in $parts) {
            $current = Join-Path $current $part
            if (-not (Test-Path -LiteralPath $current)) { break }
            $attributes = [IO.File]::GetAttributes($current)
            if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                Throw-ProbeFailure $FailureClass
            }
        }
    } catch {
        if ($_.Exception.Data.Contains('failure_class')) { throw }
        Throw-ProbeFailure $FailureClass
    }
}

function Resolve-PhysicalPath {
    param([string]$Path)
    try {
        Assert-NoExistingReparseComponents $Path
        $full = [IO.Path]::GetFullPath($Path)
        $suffix = [Collections.Generic.Stack[string]]::new()
        $existing = $full
        while (-not (Test-Path -LiteralPath $existing)) {
            $leaf = Split-Path -Leaf $existing
            if ([string]::IsNullOrEmpty($leaf)) {
                Throw-ProbeFailure 'invalid-private-path'
            }
            $suffix.Push($leaf)
            $existing = Split-Path -Parent $existing
        }
        if (-not $script:IsWindowsPlatform) {
            $resolved = (& readlink -f -- $existing).Trim()
            if ([string]::IsNullOrWhiteSpace($resolved)) {
                Throw-ProbeFailure 'invalid-private-path'
            }
        } else {
            if (-not ('KatanaProbePath.Native' -as [type])) {
                Add-Type -Namespace KatanaProbePath -Name Native -MemberDefinition @'
[DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
public static extern Microsoft.Win32.SafeHandles.SafeFileHandle CreateFile(
    string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr templateFile);
[DllImport("kernel32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
public static extern uint GetFinalPathNameByHandle(
    Microsoft.Win32.SafeHandles.SafeFileHandle handle, System.Text.StringBuilder path, uint size, uint flags);
'@
            }
            $handle = [KatanaProbePath.Native]::CreateFile(
                $existing, 0x80, 7, [IntPtr]::Zero, 3, 0x02000000, [IntPtr]::Zero)
            if ($handle.IsInvalid) {
                Throw-ProbeFailure 'invalid-private-path'
            }
            try {
                $buffer = [Text.StringBuilder]::new(32768)
                $length = [KatanaProbePath.Native]::GetFinalPathNameByHandle(
                    $handle, $buffer, $buffer.Capacity, 0)
                if ($length -eq 0 -or $length -ge $buffer.Capacity) {
                    Throw-ProbeFailure 'invalid-private-path'
                }
                $resolved = $buffer.ToString()
                if ($resolved.StartsWith('\\?\UNC\')) {
                    $resolved = '\\' + $resolved.Substring(8)
                } elseif ($resolved.StartsWith('\\?\')) {
                    $resolved = $resolved.Substring(4)
                }
            } finally {
                $handle.Dispose()
            }
        }
        while ($suffix.Count -gt 0) {
            $resolved = Join-Path $resolved $suffix.Pop()
        }
        return [IO.Path]::GetFullPath($resolved)
    } catch {
        if ($_.Exception.Data.Contains('failure_class')) { throw }
        Throw-ProbeFailure 'invalid-private-path'
    }
}

function Test-PathWithinOrEqual {
    param(
        [string]$Candidate,
        [string]$Root
    )
    $comparison = if ($script:IsWindowsPlatform) {
        [StringComparison]::OrdinalIgnoreCase
    } else {
        [StringComparison]::Ordinal
    }
    $candidateFull = [IO.Path]::GetFullPath($Candidate)
    $rootFull = [IO.Path]::GetFullPath($Root)
    if ($candidateFull.Equals($rootFull, $comparison)) { return $true }
    $rootPrefix =
        $rootFull.TrimEnd([char[]]@([IO.Path]::DirectorySeparatorChar,
                                    [IO.Path]::AltDirectorySeparatorChar)) +
        [IO.Path]::DirectorySeparatorChar
    return $candidateFull.StartsWith($rootPrefix, $comparison)
}

function Assert-OutsideRepository {
    param(
        [string]$Path,
        [string]$RepositoryRoot
    )
    try {
        $lexicalCandidate = [IO.Path]::GetFullPath($Path)
        $lexicalRoot = [IO.Path]::GetFullPath($RepositoryRoot)
        $physicalCandidate = Resolve-PhysicalPath $Path
        $physicalRoot = Resolve-PhysicalPath $RepositoryRoot
        if ((Test-PathWithinOrEqual $lexicalCandidate $lexicalRoot) -or
            (Test-PathWithinOrEqual $physicalCandidate $physicalRoot)) {
            Throw-ProbeFailure 'private-path-inside-repository'
        }
        return $physicalCandidate
    } catch {
        if ($_.Exception.Data.Contains('failure_class')) { throw }
        Throw-ProbeFailure 'invalid-private-path'
    }
}

function Resolve-ConfigPath {
    param(
        [string]$Value,
        [string]$ConfigDirectory
    )
    try {
        if ([IO.Path]::IsPathRooted($Value)) {
            return [IO.Path]::GetFullPath($Value)
        }
        return [IO.Path]::GetFullPath((Join-Path $ConfigDirectory $Value))
    } catch {
        Throw-ProbeFailure 'invalid-private-path'
    }
}

function Assert-RegularNonReparseFile {
    param(
        [string]$Path,
        [string]$FailureClass
    )
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        Throw-ProbeFailure $FailureClass
    }
    try {
        $attributes = [IO.File]::GetAttributes($Path)
    } catch {
        Throw-ProbeFailure $FailureClass
    }
    if (($attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
        Throw-ProbeFailure $FailureClass
    }
}

function Assert-SafeExecutableRelativePath {
    param([string]$Path)
    if ([IO.Path]::IsPathRooted($Path) -or
        $Path.IndexOfAny([char[]]@(':', '*', '?', '"', '<', '>', '|', [char]0)) -ge 0) {
        Throw-ProbeFailure 'invalid-executable-relative-path'
    }
    $parts = @($Path -split '[\\/]')
    if ($parts.Count -eq 0 -or
        @($parts | Where-Object {
            [string]::IsNullOrWhiteSpace($_) -or $_ -eq '.' -or $_ -eq '..'
        }).Count -ne 0) {
        Throw-ProbeFailure 'invalid-executable-relative-path'
    }
}

function Get-FileSha256 {
    param([string]$Path)
    try {
        return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
    } catch {
        Throw-ProbeFailure 'artifact-hash-failed'
    }
}

function Test-LowerSha256 {
    param($Value)
    return $Value -is [string] -and [string]$Value -cmatch '^[0-9a-f]{64}$'
}

function Convert-StrictUnsignedToken {
    param(
        [string]$Value,
        [switch]$Positive,
        [switch]$UInt32
    )
    $parsed = [uint64]0
    if ([string]::IsNullOrEmpty($Value) -or
        -not [uint64]::TryParse(
            $Value,
            [Globalization.NumberStyles]::None,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$parsed) -or
        ($Positive -and $parsed -eq 0) -or
        ($UInt32 -and $parsed -gt [uint32]::MaxValue)) {
        Throw-ProbeFailure 'invalid-disc-install-recipe'
    }
    return $parsed
}

function Get-RecipeTokens {
    param([string]$Line)
    if ([string]::IsNullOrWhiteSpace($Line)) {
        Throw-ProbeFailure 'invalid-disc-install-recipe'
    }
    return @(($Line.Trim() -split '\s+') | Where-Object { $_.Length -ne 0 })
}

function Read-DiscInstallRecipeContract {
    param([string]$Path)
    Assert-RegularNonReparseFile $Path 'invalid-disc-install-recipe'
    try {
        $lines = [IO.File]::ReadAllLines($Path)
    } catch {
        Throw-ProbeFailure 'invalid-disc-install-recipe'
    }
    if ($lines.Count -lt 7) {
        Throw-ProbeFailure 'invalid-disc-install-recipe'
    }
    $header = @(Get-RecipeTokens $lines[0])
    if ($header.Count -ne 2 -or
        $header[0] -cne 'KATANA-DISC-INSTALL' -or
        (Convert-StrictUnsignedToken $header[1] -UInt32) -ne 2) {
        Throw-ProbeFailure 'invalid-disc-install-recipe'
    }
    $expectedKeys = @('job', 'descriptor', 'content', 'boot', 'tracks')
    $values = @{}
    for ($index = 0; $index -lt $expectedKeys.Count; $index++) {
        $tokens = @(Get-RecipeTokens $lines[$index + 1])
        if ($tokens.Count -ne 2 -or $tokens[0] -cne $expectedKeys[$index]) {
            Throw-ProbeFailure 'invalid-disc-install-recipe'
        }
        $values[$expectedKeys[$index]] = [string]$tokens[1]
    }
    foreach ($field in @('job', 'descriptor', 'content', 'boot')) {
        if (-not (Test-LowerSha256 $values[$field])) {
            Throw-ProbeFailure 'invalid-disc-install-recipe'
        }
    }
    $trackCount = Convert-StrictUnsignedToken $values['tracks'] -Positive -UInt32
    if ($trackCount -gt 99 -or $lines.Count -ne 6 + [int]$trackCount) {
        Throw-ProbeFailure 'invalid-disc-install-recipe'
    }
    for ($index = 0; $index -lt [int]$trackCount; $index++) {
        $tokens = @(Get-RecipeTokens $lines[$index + 6])
        if ($tokens.Count -ne 8 -or $tokens[0] -cne 'track') {
            Throw-ProbeFailure 'invalid-disc-install-recipe'
        }
        $number = Convert-StrictUnsignedToken $tokens[1] -Positive -UInt32
        [void](Convert-StrictUnsignedToken $tokens[2] -UInt32)
        $type = Convert-StrictUnsignedToken $tokens[3] -UInt32
        $sectorSize = Convert-StrictUnsignedToken $tokens[4] -Positive -UInt32
        [void](Convert-StrictUnsignedToken $tokens[5])
        [void](Convert-StrictUnsignedToken $tokens[6] -Positive)
        if ($number -ne $index + 1 -or
            $type -notin @([uint64]0, [uint64]4) -or
            $sectorSize -notin @(
                [uint64]2048,
                [uint64]2336,
                [uint64]2352,
                [uint64]2448
            ) -or
            -not (Test-LowerSha256 $tokens[7])) {
            Throw-ProbeFailure 'invalid-disc-install-recipe'
        }
    }
    return [pscustomobject]@{
        job_generation = [string]$values['job']
        content_identity = [string]$values['content']
    }
}

function Read-PortMetadata {
    param([string]$PortRoot)
    $candidates = @(
        (Join-Path $PortRoot 'generated\metadata\port-project.json'),
        (Join-Path $PortRoot 'sourcecode\generated\metadata\port-project.json')
    )
    $existing = @($candidates | Where-Object {
        Test-Path -LiteralPath $_ -PathType Leaf
    })
    if ($existing.Count -ne 1) {
        Throw-ProbeFailure 'port-metadata-not-unique'
    }
    $metadataPath = Resolve-PhysicalPath $existing[0]
    if (-not (Test-PathWithinOrEqual $metadataPath $PortRoot)) {
        Throw-ProbeFailure 'port-metadata-outside-port'
    }
    Assert-RegularNonReparseFile $metadataPath 'invalid-port-metadata'
    try {
        $text = Get-Content -LiteralPath $metadataPath -Raw
    } catch {
        Throw-ProbeFailure 'invalid-port-metadata'
    }
    $metadata = Read-JsonObject $text 'invalid-port-metadata'
    foreach ($field in @(
        'status',
        'execution_profile',
        'diagnostic_partial',
        'runtime_interpreter_enabled',
        'project_identity'
    )) {
        if ($metadata.PSObject.Properties.Name -cnotcontains $field) {
            Throw-ProbeFailure 'invalid-port-metadata'
        }
    }
    if ($metadata.status -isnot [string] -or
        [string]$metadata.status -cne 'success' -or
        $metadata.execution_profile -isnot [string] -or
        [string]$metadata.execution_profile -cne 'native-aot-product' -or
        -not (Test-JsonBoolean $metadata.diagnostic_partial) -or
        [bool]$metadata.diagnostic_partial -or
        -not (Test-JsonBoolean $metadata.runtime_interpreter_enabled) -or
        [bool]$metadata.runtime_interpreter_enabled -or
        -not (Test-LowerSha256 $metadata.project_identity)) {
        Throw-ProbeFailure 'port-not-native-aot-product'
    }
    return $metadata
}

function Assert-PortInstallBinding {
    param(
        [string]$PortRoot,
        $Metadata,
        $ExecutableEntry
    )
    $contentRoot = Join-Path $PortRoot 'content'
    $recipePath = Resolve-PhysicalPath (
        [IO.Path]::GetFullPath((Join-Path $contentRoot 'game.katana-install')))
    $manifestPath = Resolve-PhysicalPath (
        [IO.Path]::GetFullPath((Join-Path $contentRoot 'game.katana-install.json')))
    foreach ($path in @($recipePath, $manifestPath)) {
        if (-not (Test-PathWithinOrEqual $path $PortRoot)) {
            Throw-ProbeFailure 'port-install-artifact-outside-port'
        }
    }
    Assert-RegularNonReparseFile $manifestPath 'invalid-disc-install-manifest'
    $recipe = Read-DiscInstallRecipeContract $recipePath
    if ($recipe.job_generation -cne [string]$Metadata.project_identity) {
        Throw-ProbeFailure 'port-install-identity-mismatch'
    }
    try {
        $manifestText = Get-Content -LiteralPath $manifestPath -Raw
    } catch {
        Throw-ProbeFailure 'invalid-disc-install-manifest'
    }
    $manifest = Read-JsonObject $manifestText 'invalid-disc-install-manifest'
    Assert-ExactFields $manifest @(
        'schema',
        'version',
        'job_generation',
        'content_identity',
        'artifacts'
    ) @() 'invalid-disc-install-manifest'
    if ($manifest.schema -isnot [string] -or
        [string]$manifest.schema -cne 'katana-disc-install' -or
        -not (Test-IntegralJsonNumber $manifest.version) -or
        [decimal]$manifest.version -ne 1 -or
        -not (Test-LowerSha256 $manifest.job_generation) -or
        -not (Test-LowerSha256 $manifest.content_identity) -or
        [string]$manifest.job_generation -cne [string]$Metadata.project_identity -or
        [string]$manifest.content_identity -cne [string]$recipe.content_identity -or
        $manifest.artifacts -isnot [array] -or
        @($manifest.artifacts).Count -ne 2) {
        Throw-ProbeFailure 'invalid-disc-install-manifest'
    }

    $roles = @{}
    foreach ($artifact in @($manifest.artifacts)) {
        Assert-ExactFields $artifact @(
            'role',
            'path',
            'sha256'
        ) @() 'invalid-disc-install-manifest'
        if ($artifact.role -isnot [string] -or
            $artifact.path -isnot [string] -or
            -not (Test-LowerSha256 $artifact.sha256) -or
            [string]::IsNullOrWhiteSpace([string]$artifact.path) -or
            [IO.Path]::IsPathRooted([string]$artifact.path) -or
            ([string]$artifact.path).IndexOfAny(
                [char[]]@(':', '*', '?', '"', '<', '>', '|', [char]0)) -ge 0) {
            Throw-ProbeFailure 'invalid-disc-install-manifest'
        }
        $role = [string]$artifact.role
        if (@('disc_install_recipe', 'host_executable') -cnotcontains $role -or
            $roles.ContainsKey($role)) {
            Throw-ProbeFailure 'invalid-disc-install-manifest'
        }
        try {
            $artifactLexical = [IO.Path]::GetFullPath(
                (Join-Path (Split-Path -Parent $manifestPath) ([string]$artifact.path)))
        } catch {
            Throw-ProbeFailure 'invalid-disc-install-manifest'
        }
        if (-not (Test-PathWithinOrEqual $artifactLexical $PortRoot)) {
            Throw-ProbeFailure 'port-install-artifact-outside-port'
        }
        $artifactPhysical = Resolve-PhysicalPath $artifactLexical
        if (-not (Test-PathWithinOrEqual $artifactPhysical $PortRoot)) {
            Throw-ProbeFailure 'port-install-artifact-outside-port'
        }
        Assert-RegularNonReparseFile $artifactPhysical 'invalid-disc-install-manifest'
        $roles[$role] = [pscustomobject]@{
            path = $artifactPhysical
            sha256 = [string]$artifact.sha256
        }
    }
    if (-not $roles.ContainsKey('disc_install_recipe') -or
        -not $roles.ContainsKey('host_executable')) {
        Throw-ProbeFailure 'invalid-disc-install-manifest'
    }
    $comparison = if ($script:IsWindowsPlatform) {
        [StringComparison]::OrdinalIgnoreCase
    } else {
        [StringComparison]::Ordinal
    }
    if (-not ([string]$roles['disc_install_recipe'].path).Equals(
            [string]$recipePath,
            $comparison) -or
        -not ([string]$roles['host_executable'].path).Equals(
            [string]$ExecutableEntry.source,
            $comparison) -or
        [string]$roles['disc_install_recipe'].sha256 -cne
            (Get-FileSha256 $recipePath) -or
        [string]$roles['host_executable'].sha256 -cne
            [string]$ExecutableEntry.sha256) {
        Throw-ProbeFailure 'port-install-artifact-binding-mismatch'
    }
}

function Get-ContainedRelativePath {
    param(
        [string]$Candidate,
        [string]$Root
    )
    $candidateFull = [IO.Path]::GetFullPath($Candidate)
    $rootFull = [IO.Path]::GetFullPath($Root)
    if (-not (Test-PathWithinOrEqual $candidateFull $rootFull) -or
        $candidateFull.Length -le $rootFull.Length) {
        Throw-ProbeFailure 'runtime-dependency-outside-port'
    }
    $relative = $candidateFull.Substring($rootFull.Length)
    $relative = $relative.TrimStart([char[]]@(
        [IO.Path]::DirectorySeparatorChar,
        [IO.Path]::AltDirectorySeparatorChar))
    if ([string]::IsNullOrWhiteSpace($relative)) {
        Throw-ProbeFailure 'invalid-runtime-dependency'
    }
    return $relative
}

function Get-RuntimeInventory {
    param(
        [string]$PortRoot,
        [string]$ExecutableRelative,
        $Metadata
    )
    Assert-SafeExecutableRelativePath $ExecutableRelative
    $executableLexical = [IO.Path]::GetFullPath((Join-Path $PortRoot $ExecutableRelative))
    if (-not (Test-PathWithinOrEqual $executableLexical $PortRoot)) {
        Throw-ProbeFailure 'executable-outside-port'
    }
    $executable = Resolve-PhysicalPath $executableLexical
    if (-not (Test-PathWithinOrEqual $executable $PortRoot)) {
        Throw-ProbeFailure 'executable-outside-port'
    }
    Assert-RegularNonReparseFile $executable 'invalid-executable'

    $runtimeManifestLexical =
        [IO.Path]::GetFullPath((Join-Path $PortRoot 'runtime\runtime-dependencies.json'))
    $runtimeManifest = Resolve-PhysicalPath $runtimeManifestLexical
    if (-not (Test-PathWithinOrEqual $runtimeManifest $PortRoot)) {
        Throw-ProbeFailure 'runtime-dependency-outside-port'
    }
    Assert-RegularNonReparseFile $runtimeManifest 'invalid-runtime-manifest'
    try {
        $manifestText = Get-Content -LiteralPath $runtimeManifest -Raw
    } catch {
        Throw-ProbeFailure 'invalid-runtime-manifest'
    }
    $manifest = Read-JsonObject $manifestText 'invalid-runtime-manifest'
    Assert-ExactFields $manifest @(
        'schema',
        'version',
        'linkage',
        'job_generation',
        'files'
    ) @() 'invalid-runtime-manifest'
    if ($manifest.schema -isnot [string] -or
        [string]$manifest.schema -cne 'katana-runtime-dependencies' -or
        -not (Test-IntegralJsonNumber $manifest.version) -or
        [decimal]$manifest.version -ne 1 -or
        $manifest.linkage -isnot [string] -or
        [string]$manifest.linkage -cne 'static' -or
        -not (Test-LowerSha256 $manifest.job_generation) -or
        [string]$manifest.job_generation -cne [string]$Metadata.project_identity -or
        $manifest.files -isnot [array] -or
        @($manifest.files).Count -ne 0) {
        Throw-ProbeFailure 'invalid-runtime-manifest'
    }

    $candidates = [Collections.Generic.List[object]]::new()
    $candidates.Add([pscustomobject]@{
        source = $executable
        relative = Get-ContainedRelativePath $executableLexical $PortRoot
        expected_sha256 = $null
        role = 'executable'
    })
    $candidates.Add([pscustomobject]@{
        source = $runtimeManifest
        relative = Get-ContainedRelativePath $runtimeManifestLexical $PortRoot
        expected_sha256 = $null
        role = 'runtime-manifest'
    })

    $manifestDirectory = Split-Path -Parent $runtimeManifestLexical
    foreach ($file in @($manifest.files)) {
        $relativeValue = $null
        $expectedSha256 = $null
        if ($file -is [string]) {
            $relativeValue = [string]$file
        } elseif ($file -is [pscustomobject]) {
            Assert-ExactFields $file @('path') @('sha256') 'invalid-runtime-manifest'
            if ($file.path -isnot [string]) {
                Throw-ProbeFailure 'invalid-runtime-manifest'
            }
            $relativeValue = [string]$file.path
            if ($file.PSObject.Properties.Name -ccontains 'sha256') {
                if ($file.sha256 -isnot [string] -or
                    [string]$file.sha256 -cnotmatch '^[0-9a-f]{64}$') {
                    Throw-ProbeFailure 'invalid-runtime-manifest'
                }
                $expectedSha256 = [string]$file.sha256
            }
        } else {
            Throw-ProbeFailure 'invalid-runtime-manifest'
        }
        if ([string]::IsNullOrWhiteSpace($relativeValue) -or
            [IO.Path]::IsPathRooted($relativeValue) -or
            $relativeValue.IndexOfAny([char[]]@(':', '*', '?', '"', '<', '>', '|', [char]0)) -ge 0) {
            Throw-ProbeFailure 'invalid-runtime-manifest'
        }
        try {
            $dependencyLexical =
                [IO.Path]::GetFullPath((Join-Path $manifestDirectory $relativeValue))
        } catch {
            Throw-ProbeFailure 'invalid-runtime-manifest'
        }
        if (-not (Test-PathWithinOrEqual $dependencyLexical $PortRoot)) {
            Throw-ProbeFailure 'runtime-dependency-outside-port'
        }
        $dependency = Resolve-PhysicalPath $dependencyLexical
        if (-not (Test-PathWithinOrEqual $dependency $PortRoot)) {
            Throw-ProbeFailure 'runtime-dependency-outside-port'
        }
        Assert-RegularNonReparseFile $dependency 'invalid-runtime-dependency'
        $candidates.Add([pscustomobject]@{
            source = $dependency
            relative = Get-ContainedRelativePath $dependencyLexical $PortRoot
            expected_sha256 = $expectedSha256
            role = 'runtime-dependency'
        })
    }

    $byRelative = @{}
    foreach ($candidate in $candidates) {
        $portable = ([string]$candidate.relative).Replace('\', '/')
        if ($portable -match '(?i)^user-data(?:/|$)' -or
            $portable -match '(?i)\.katana-disc$') {
            Throw-ProbeFailure 'retail-or-user-data-runtime-dependency'
        }
        $key = $portable.ToLowerInvariant()
        $actualSha256 = Get-FileSha256 ([string]$candidate.source)
        if ($null -ne $candidate.expected_sha256 -and
            $actualSha256 -cne [string]$candidate.expected_sha256) {
            Throw-ProbeFailure 'runtime-dependency-hash-mismatch'
        }
        if ($byRelative.ContainsKey($key)) {
            $existing = $byRelative[$key]
            if ([string]$existing.source -cne [string]$candidate.source -or
                [string]$existing.sha256 -cne $actualSha256) {
                Throw-ProbeFailure 'duplicate-runtime-dependency'
            }
            continue
        }
        $byRelative[$key] = [pscustomobject]@{
            source = [string]$candidate.source
            relative = $portable
            sha256 = $actualSha256
            role = [string]$candidate.role
        }
    }
    return @($byRelative.Values | Sort-Object -Property relative)
}

function Assert-InventoryUnchanged {
    param([object[]]$Inventory)
    foreach ($entry in $Inventory) {
        Assert-RegularNonReparseFile ([string]$entry.source) 'runtime-artifact-changed'
        if ((Get-FileSha256 ([string]$entry.source)) -cne [string]$entry.sha256) {
            Throw-ProbeFailure 'runtime-artifact-changed'
        }
    }
}

function New-FreshRuntimeRoot {
    param(
        [string]$OutputRoot,
        [object[]]$Inventory,
        [string]$ExecutableRelative,
        [string]$Label
    )
    $runRoot = Join-Path $OutputRoot (
        'runtime-probe-' + $Label + '-' + [guid]::NewGuid().ToString('N'))
    if (Test-Path -LiteralPath $runRoot) {
        Throw-ProbeFailure 'runtime-root-not-fresh'
    }
    [void][IO.Directory]::CreateDirectory($runRoot)
    foreach ($entry in $Inventory) {
        $relativeNative =
            ([string]$entry.relative).Replace('/', [IO.Path]::DirectorySeparatorChar)
        $destination = [IO.Path]::GetFullPath((Join-Path $runRoot $relativeNative))
        if (-not (Test-PathWithinOrEqual $destination $runRoot) -or
            $destination.Equals(
                [IO.Path]::GetFullPath($runRoot),
                [StringComparison]::OrdinalIgnoreCase)) {
            Throw-ProbeFailure 'invalid-runtime-copy-destination'
        }
        [void][IO.Directory]::CreateDirectory((Split-Path -Parent $destination))
        try {
            [IO.File]::Copy([string]$entry.source, $destination, $false)
        } catch {
            Throw-ProbeFailure 'runtime-copy-failed'
        }
        if ((Get-FileSha256 $destination) -cne [string]$entry.sha256) {
            Throw-ProbeFailure 'runtime-copy-hash-mismatch'
        }
    }
    $executable = [IO.Path]::GetFullPath((Join-Path $runRoot $ExecutableRelative))
    Assert-RegularNonReparseFile $executable 'runtime-copy-missing-executable'
    $userData = Join-Path (Split-Path -Parent $executable) 'user-data'
    if (Test-Path -LiteralPath $userData) {
        Throw-ProbeFailure 'runtime-user-data-not-fresh'
    }
    [void][IO.Directory]::CreateDirectory($userData)
    if (@([IO.Directory]::EnumerateFileSystemEntries($userData)).Count -ne 0) {
        Throw-ProbeFailure 'runtime-user-data-not-empty'
    }
    return [pscustomobject]@{
        root = $runRoot
        executable = $executable
        executable_sha256 = Get-FileSha256 $executable
    }
}

function Get-SanitizedBaseEnvironment {
    $environment = @{}
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) {
        $name = [string]$entry.Key
        if ($name.StartsWith('KATANA_', [StringComparison]::OrdinalIgnoreCase)) {
            continue
        }
        $environment[$name] = [string]$entry.Value
    }
    return $environment
}

function ConvertTo-CommandLineArgument {
    param([AllowEmptyString()][string]$Value)
    if ($Value.Length -ne 0 -and $Value -notmatch '[\s"]') {
        return $Value
    }
    $builder = [Text.StringBuilder]::new()
    [void]$builder.Append('"')
    $backslashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ([int]$character -eq 92) {
            $backslashes++
            continue
        }
        if ([int]$character -eq 34) {
            if ($backslashes -gt 0) {
                [void]$builder.Append(('\' * ($backslashes * 2)))
            }
            [void]$builder.Append('\')
            [void]$builder.Append('"')
            $backslashes = 0
            continue
        }
        if ($backslashes -gt 0) {
            [void]$builder.Append(('\' * $backslashes))
            $backslashes = 0
        }
        [void]$builder.Append($character)
    }
    if ($backslashes -gt 0) {
        [void]$builder.Append(('\' * ($backslashes * 2)))
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function Initialize-RuntimeProcessSupport {
    if ('KatanaProbeProcess.RuntimeSupport' -as [type]) { return }
    if (-not $script:IsWindowsPlatform) {
        Throw-ProbeFailure 'windows-job-object-required'
    }
    try {
        Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading.Tasks;

namespace KatanaProbeProcess
{
    [StructLayout(LayoutKind.Sequential)]
    internal struct IoCounters
    {
        public ulong ReadOperationCount;
        public ulong WriteOperationCount;
        public ulong OtherOperationCount;
        public ulong ReadTransferCount;
        public ulong WriteTransferCount;
        public ulong OtherTransferCount;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct BasicLimitInformation
    {
        public long PerProcessUserTimeLimit;
        public long PerJobUserTimeLimit;
        public uint LimitFlags;
        public UIntPtr MinimumWorkingSetSize;
        public UIntPtr MaximumWorkingSetSize;
        public uint ActiveProcessLimit;
        public IntPtr Affinity;
        public uint PriorityClass;
        public uint SchedulingClass;
    }

    [StructLayout(LayoutKind.Sequential)]
    internal struct ExtendedLimitInformation
    {
        public BasicLimitInformation BasicLimitInformation;
        public IoCounters IoInfo;
        public UIntPtr ProcessMemoryLimit;
        public UIntPtr JobMemoryLimit;
        public UIntPtr PeakProcessMemoryUsed;
        public UIntPtr PeakJobMemoryUsed;
    }

    public static class RuntimeSupport
    {
        private const uint JobObjectLimitKillOnJobClose = 0x00002000;
        private const int JobObjectExtendedLimitInformation = 9;

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateJobObject(IntPtr securityAttributes, string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool SetInformationJobObject(
            IntPtr job,
            int informationClass,
            ref ExtendedLimitInformation information,
            uint informationLength);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr handle);

        public static IntPtr CreateKillOnCloseJob()
        {
            IntPtr job = CreateJobObject(IntPtr.Zero, null);
            if (job == IntPtr.Zero)
                throw new Win32Exception(Marshal.GetLastWin32Error());
            ExtendedLimitInformation information = new ExtendedLimitInformation();
            information.BasicLimitInformation.LimitFlags = JobObjectLimitKillOnJobClose;
            uint size = checked((uint)Marshal.SizeOf(typeof(ExtendedLimitInformation)));
            if (!SetInformationJobObject(
                    job,
                    JobObjectExtendedLimitInformation,
                    ref information,
                    size))
            {
                int error = Marshal.GetLastWin32Error();
                CloseHandle(job);
                throw new Win32Exception(error);
            }
            return job;
        }

        public static void Assign(IntPtr job, IntPtr process)
        {
            if (job == IntPtr.Zero || process == IntPtr.Zero ||
                !AssignProcessToJobObject(job, process))
                throw new Win32Exception(Marshal.GetLastWin32Error());
        }

        public static bool CloseKillOnCloseJob(IntPtr job)
        {
            return job == IntPtr.Zero || CloseHandle(job);
        }

        public static async Task<string> ReadCappedAsync(
            TextReader reader,
            int maximumCharacters)
        {
            if (reader == null)
                throw new ArgumentNullException("reader");
            if (maximumCharacters <= 0)
                throw new ArgumentOutOfRangeException("maximumCharacters");
            StringBuilder output = new StringBuilder(
                Math.Min(maximumCharacters, 16 * 1024));
            char[] buffer = new char[4096];
            for (;;)
            {
                int read = await reader.ReadAsync(buffer, 0, buffer.Length)
                    .ConfigureAwait(false);
                if (read == 0)
                    return output.ToString();
                if (read > maximumCharacters - output.Length)
                    throw new InvalidDataException("capture-limit-exceeded");
                output.Append(buffer, 0, read);
            }
        }
    }
}
'@ -Language CSharp
    } catch {
        Throw-ProbeFailure 'runtime-process-support-unavailable'
    }
}

function Close-RuntimeJob {
    param([IntPtr]$Handle)
    if ($Handle -eq [IntPtr]::Zero) { return }
    try {
        if (-not [KatanaProbeProcess.RuntimeSupport]::CloseKillOnCloseJob($Handle)) {
            Throw-ProbeFailure 'runtime-job-close-failed'
        }
    } catch {
        if ($_.Exception.Data.Contains('failure_class')) { throw }
        Throw-ProbeFailure 'runtime-job-close-failed'
    }
}

function New-RuntimeProcessStartInfo {
    param(
        [string]$Executable,
        [string[]]$Arguments,
        [string]$WorkingDirectory,
        [hashtable]$BaseEnvironment,
        [uint64]$GuestCycleBudget,
        [bool]$DiagnosticsEnabled
    )
    Initialize-RuntimeProcessSupport
    $gateName =
        'Local\KatanaRuntimeProbe-' + [guid]::NewGuid().ToString('N')
    try {
        $gate = [Threading.EventWaitHandle]::new(
            $false,
            [Threading.EventResetMode]::ManualReset,
            $gateName)
    } catch {
        Throw-ProbeFailure 'runtime-launch-gate-failed'
    }
    $payload = [ordered]@{
        executable = $Executable
        arguments = @($Arguments)
    } | ConvertTo-Json -Compress
    $payloadBase64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($payload))
    $wrapperTemplate = @'
$ErrorActionPreference = 'Stop'
$payloadText = [Text.Encoding]::UTF8.GetString(
    [Convert]::FromBase64String('__PAYLOAD__'))
$payload = $payloadText | ConvertFrom-Json
$gate = [Threading.EventWaitHandle]::OpenExisting('__GATE__')
try {
    if (-not $gate.WaitOne(30000)) { exit 125 }
} finally {
    $gate.Dispose()
}
$runtimeArguments = @($payload.arguments | ForEach-Object { [string]$_ })
& ([string]$payload.executable) @runtimeArguments
$runtimeExitCode = $LASTEXITCODE
if ($null -eq $runtimeExitCode) { exit 126 }
exit [int]$runtimeExitCode
'@
    $wrapper = $wrapperTemplate.Replace('__PAYLOAD__', $payloadBase64).
        Replace('__GATE__', $gateName)
    $encodedWrapper =
        [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($wrapper))
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.FileName = [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    $start.WorkingDirectory = $WorkingDirectory
    $start.Arguments =
        '-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand ' +
        $encodedWrapper
    # Windows PowerShell 5.1 initialisiert dieses Dictionary beim ersten Getter,
    # liefert bei genau diesem Zugriff aber null zurueck.
    $null = $start.EnvironmentVariables
    $childEnvironment = $start.EnvironmentVariables
    if ($null -eq $childEnvironment) {
        Throw-ProbeFailure 'runtime-environment-unavailable'
    }
    $childEnvironment.Clear()
    foreach ($key in @($BaseEnvironment.Keys | Sort-Object)) {
        $childEnvironment[[string]$key] = [string]$BaseEnvironment[$key]
    }
    $childEnvironment['KATANA_RUNTIME_PROBE'] = $script:ProbeProfile
    $childEnvironment['KATANA_GUEST_CYCLE_BUDGET'] =
        $GuestCycleBudget.ToString([Globalization.CultureInfo]::InvariantCulture)
    $childEnvironment['KATANA_PORT_DIAGNOSTICS'] =
        if ($DiagnosticsEnabled) { '1' } else { '0' }

    $katanaKeys = @($childEnvironment.Keys | Where-Object {
        ([string]$_).StartsWith('KATANA_', [StringComparison]::OrdinalIgnoreCase)
    })
    $expected = @(
        'KATANA_GUEST_CYCLE_BUDGET',
        'KATANA_PORT_DIAGNOSTICS',
        'KATANA_RUNTIME_PROBE'
    )
    if ($katanaKeys.Count -ne $expected.Count -or
        @($katanaKeys | Where-Object { $expected -cnotcontains [string]$_ }).Count -ne 0) {
        $gate.Dispose()
        Throw-ProbeFailure 'runtime-environment-not-sanitized'
    }
    return [pscustomobject]@{
        start_info = $start
        gate = $gate
    }
}

function Stop-HardProcessTree {
    param([Diagnostics.Process]$Process)
    try {
        if ($Process.HasExited) { return }
    } catch {
        return
    }
    if ($script:IsWindowsPlatform) {
        $taskkillPath = Join-Path $env:SystemRoot 'System32\taskkill.exe'
        if (Test-Path -LiteralPath $taskkillPath -PathType Leaf) {
            $killStart = [Diagnostics.ProcessStartInfo]::new()
            $killStart.UseShellExecute = $false
            $killStart.CreateNoWindow = $true
            $killStart.RedirectStandardOutput = $true
            $killStart.RedirectStandardError = $true
            $killStart.FileName = $taskkillPath
            $killStart.Arguments = '/PID ' + $Process.Id + ' /T /F'
            try {
                $killer = [Diagnostics.Process]::Start($killStart)
                $killOut = [KatanaProbeProcess.RuntimeSupport]::ReadCappedAsync(
                    $killer.StandardOutput,
                    4096)
                $killError = [KatanaProbeProcess.RuntimeSupport]::ReadCappedAsync(
                    $killer.StandardError,
                    4096)
                if (-not $killer.WaitForExit(5000)) {
                    try { $killer.Kill() } catch {}
                }
                try { [void]$killOut.Wait(1000) } catch {}
                try { [void]$killError.Wait(1000) } catch {}
                $killer.Dispose()
            } catch {
                # The root-process fallback below is still bounded.
            }
        }
    } else {
        try {
            $null = & kill -KILL $Process.Id 2>$null
        } catch {}
    }
    try {
        if (-not $Process.HasExited) { $Process.Kill() }
    } catch {}
}

function Invoke-BudgetedRuntime {
    param(
        [string]$Executable,
        [string[]]$Arguments,
        [string]$WorkingDirectory,
        [hashtable]$BaseEnvironment,
        [uint64]$GuestCycleBudget,
        [bool]$DiagnosticsEnabled,
        [int]$TimeoutSeconds,
        [int]$MaximumCaptureCharacters = $script:MaximumCaptureCharacters
    )
    if ($MaximumCaptureCharacters -lt 1 -or
        $MaximumCaptureCharacters -gt $script:MaximumCaptureCharacters) {
        Throw-ProbeFailure 'invalid-runtime-capture-limit'
    }
    $launch = New-RuntimeProcessStartInfo $Executable $Arguments $WorkingDirectory `
        $BaseEnvironment $GuestCycleBudget $DiagnosticsEnabled
    $start = $launch.start_info
    $process = $null
    $stdoutTask = $null
    $stderrTask = $null
    $job = [IntPtr]::Zero
    $jobClosed = $false
    $clock = [Diagnostics.Stopwatch]::StartNew()
    try {
        try {
            $job = [KatanaProbeProcess.RuntimeSupport]::CreateKillOnCloseJob()
        } catch {
            Throw-ProbeFailure 'runtime-job-create-failed'
        }
        try {
            $process = [Diagnostics.Process]::Start($start)
        } catch {
            Throw-ProbeFailure 'runtime-start-failed'
        }
        try {
            [KatanaProbeProcess.RuntimeSupport]::Assign($job, $process.Handle)
        } catch {
            Close-RuntimeJob $job
            $jobClosed = $true
            $job = [IntPtr]::Zero
            Stop-HardProcessTree $process
            Throw-ProbeFailure 'runtime-job-assignment-failed'
        }
        $stdoutTask = [KatanaProbeProcess.RuntimeSupport]::ReadCappedAsync(
            $process.StandardOutput,
            $MaximumCaptureCharacters)
        $stderrTask = [KatanaProbeProcess.RuntimeSupport]::ReadCappedAsync(
            $process.StandardError,
            $MaximumCaptureCharacters)
        if (-not $launch.gate.Set()) {
            Throw-ProbeFailure 'runtime-launch-gate-failed'
        }

        $deadlineMilliseconds = [int64]$TimeoutSeconds * 1000
        $finished = $false
        $timedOut = $false
        $captureFailed = $false
        while (-not $finished -and -not $timedOut -and -not $captureFailed) {
            if ($stdoutTask.IsFaulted -or $stdoutTask.IsCanceled -or
                $stderrTask.IsFaulted -or $stderrTask.IsCanceled) {
                $captureFailed = $true
                break
            }
            if ($process.HasExited) {
                $finished = $true
                break
            }
            $remaining = $deadlineMilliseconds - $clock.ElapsedMilliseconds
            if ($remaining -le 0) {
                $timedOut = $true
                break
            }
            [void]$process.WaitForExit([int][Math]::Min(50, $remaining))
        }
        $rootExitCode = if ($finished) { [int]$process.ExitCode } else { $null }

        # Closing this KILL_ON_JOB_CLOSE handle ends the root and every descendant,
        # including descendants left behind after an otherwise successful root exit.
        Close-RuntimeJob $job
        $jobClosed = $true
        $job = [IntPtr]::Zero
        if (-not $process.HasExited) {
            [void]$process.WaitForExit(5000)
        }
        if (-not $process.HasExited) {
            Stop-HardProcessTree $process
            [void]$process.WaitForExit(5000)
        }
        if (-not $process.HasExited) {
            Throw-ProbeFailure 'runtime-process-tree-survived'
        }

        $drainDeadline = [Diagnostics.Stopwatch]::StartNew()
        while ((-not $stdoutTask.IsCompleted -or -not $stderrTask.IsCompleted) -and
               $drainDeadline.ElapsedMilliseconds -lt 5000) {
            Start-Sleep -Milliseconds 10
        }
        $drainDeadline.Stop()
        if (-not $stdoutTask.IsCompleted -or -not $stderrTask.IsCompleted) {
            if (-not $jobClosed) {
                Close-RuntimeJob $job
                $jobClosed = $true
                $job = [IntPtr]::Zero
            }
            Stop-HardProcessTree $process
            Throw-ProbeFailure 'runtime-output-drain-timeout'
        }
        if ($stdoutTask.IsFaulted -or $stderrTask.IsFaulted) {
            $messages = @()
            foreach ($task in @($stdoutTask, $stderrTask)) {
                if ($task.IsFaulted -and $null -ne $task.Exception) {
                    $messages += $task.Exception.GetBaseException().Message
                }
            }
            if ($messages -contains 'capture-limit-exceeded') {
                Throw-ProbeFailure 'runtime-output-too-large'
            }
            Throw-ProbeFailure 'runtime-output-read-failed'
        }
        if ($stdoutTask.IsCanceled -or $stderrTask.IsCanceled) {
            Throw-ProbeFailure 'runtime-output-read-failed'
        }
        $stdout = [string]$stdoutTask.Result
        $stderr = [string]$stderrTask.Result
        return [pscustomobject]@{
            timed_out = $timedOut
            exit_code = $rootExitCode
            stdout = $stdout
            stderr = $stderr
            elapsed_milliseconds = $clock.ElapsedMilliseconds
        }
    } finally {
        $clock.Stop()
        if (-not $jobClosed -and $job -ne [IntPtr]::Zero) {
            try { Close-RuntimeJob $job } catch {}
            $jobClosed = $true
            $job = [IntPtr]::Zero
        }
        if ($null -ne $process) {
            try {
                if (-not $process.HasExited) { Stop-HardProcessTree $process }
            } catch {}
            $process.Dispose()
        }
        if ($null -ne $launch -and $null -ne $launch.gate) {
            $launch.gate.Dispose()
        }
    }
}

function Get-OutputLines {
    param([AllowEmptyString()][string]$Text)
    if ([string]::IsNullOrEmpty($Text)) { return @() }
    return @([regex]::Split($Text, '\r\n|\n|\r'))
}

function Get-RuntimeCheckpointState {
    param(
        [AllowEmptyString()][string]$Stdout,
        [AllowEmptyString()][string]$Stderr
    )
    $stdoutLines = @(Get-OutputLines $Stdout)
    $checkpointLines = @($stdoutLines | Where-Object {
        $_.StartsWith($script:CheckpointMarker, [StringComparison]::Ordinal)
    })
    $stdoutOccurrences = [regex]::Matches(
        $Stdout,
        [regex]::Escape($script:CheckpointMarker)).Count
    $stderrOccurrences = [regex]::Matches(
        $Stderr,
        [regex]::Escape($script:CheckpointMarker)).Count
    if ($stdoutOccurrences -ne $checkpointLines.Count -or
        $stderrOccurrences -ne 0) {
        Throw-ProbeFailure 'invalid-runtime-checkpoint-lines'
    }
    $allowedCheckpoints = @(
        'runtime-started',
        'guest-program-entered',
        'first-guest-frame',
        'guest-input-interactive',
        'controlled-retail-scene'
    )
    [uint64]$expectedSequence = 1
    $lastRank = -1
    $lastCheckpoint = $null
    foreach ($line in $checkpointLines) {
        $payload = $line.Substring($script:CheckpointMarker.Length)
        if ([string]::IsNullOrWhiteSpace($payload) -or $payload.Length -gt 65536) {
            Throw-ProbeFailure 'invalid-runtime-checkpoint-json'
        }
        $checkpoint = Read-JsonObject $payload 'invalid-runtime-checkpoint-json'
        Assert-ExactFields $checkpoint @(
            'schema',
            'report_version',
            'status',
            'sequence',
            'checkpoint'
        ) @() 'invalid-runtime-checkpoint-contract'
        $sequence = Get-StrictUInt64 $checkpoint.sequence -Positive
        if ($checkpoint.schema -isnot [string] -or
            [string]$checkpoint.schema -cne 'katana.runtime-probe-checkpoint' -or
            -not (Test-IntegralJsonNumber $checkpoint.report_version) -or
            [decimal]$checkpoint.report_version -ne 1 -or
            $checkpoint.status -isnot [string] -or
            [string]$checkpoint.status -cne 'observed' -or
            $checkpoint.checkpoint -isnot [string] -or
            $allowedCheckpoints -cnotcontains [string]$checkpoint.checkpoint -or
            $sequence -ne $expectedSequence) {
            Throw-ProbeFailure 'invalid-runtime-checkpoint-contract'
        }
        $rank = [Array]::IndexOf(
            [string[]]$allowedCheckpoints,
            [string]$checkpoint.checkpoint)
        if ($rank -le $lastRank) {
            Throw-ProbeFailure 'invalid-runtime-checkpoint-order'
        }
        $lastRank = $rank
        $lastCheckpoint = [string]$checkpoint.checkpoint
        ++$expectedSequence
    }
    return [pscustomobject]@{
        present = $checkpointLines.Count -ne 0
        checkpoint = $lastCheckpoint
        count = [uint64]$checkpointLines.Count
    }
}

function Read-RuntimeFaultLine {
    param(
        [AllowEmptyString()][string]$Stdout,
        [AllowEmptyString()][string]$Stderr
    )
    $stdoutLines = @(Get-OutputLines $Stdout)
    $faultLines = @($stdoutLines | Where-Object {
        $_.StartsWith($script:FaultMarker, [StringComparison]::Ordinal)
    })
    if ($faultLines.Count -ne 1 -or
        [regex]::Matches(
            $Stdout,
            [regex]::Escape($script:FaultMarker)).Count -ne 1 -or
        [regex]::Matches(
            $Stderr,
            [regex]::Escape($script:FaultMarker)).Count -ne 0) {
        Throw-ProbeFailure 'runtime-fault-line-count'
    }
    $payload = $faultLines[0].Substring($script:FaultMarker.Length)
    if ([string]::IsNullOrWhiteSpace($payload) -or $payload.Length -gt 65536) {
        Throw-ProbeFailure 'invalid-runtime-fault-json'
    }
    $fault = Read-JsonObject $payload 'invalid-runtime-fault-json'
    Assert-ExactFields $fault @(
        'schema',
        'report_version',
        'termination',
        'first_fault_present',
        'first_fault',
        'last_checkpoint_present',
        'last_checkpoint'
    ) @() 'invalid-runtime-fault-contract'
    $allowedFaults = @('hang', 'guest-exception', 'dispatch-miss', 'failed')
    $allowedCheckpoints = @(
        'runtime-started',
        'guest-program-entered',
        'first-guest-frame',
        'guest-input-interactive',
        'controlled-retail-scene'
    )
    if ($fault.schema -isnot [string] -or
        [string]$fault.schema -cne 'katana.runtime-probe-fault' -or
        -not (Test-IntegralJsonNumber $fault.report_version) -or
        [decimal]$fault.report_version -ne 1 -or
        $fault.termination -isnot [string] -or
        $allowedFaults -cnotcontains [string]$fault.termination -or
        -not (Test-JsonBoolean $fault.first_fault_present) -or
        -not [bool]$fault.first_fault_present -or
        $fault.first_fault -isnot [string] -or
        $allowedFaults -cnotcontains [string]$fault.first_fault -or
        [string]$fault.termination -cne [string]$fault.first_fault -or
        -not (Test-JsonBoolean $fault.last_checkpoint_present)) {
        Throw-ProbeFailure 'invalid-runtime-fault-contract'
    }
    $checkpointPresent = [bool]$fault.last_checkpoint_present
    if (($checkpointPresent -and (
            $fault.last_checkpoint -isnot [string] -or
            $allowedCheckpoints -cnotcontains [string]$fault.last_checkpoint)) -or
        (-not $checkpointPresent -and $null -ne $fault.last_checkpoint)) {
        Throw-ProbeFailure 'invalid-runtime-fault-contract'
    }
    return [pscustomobject]@{
        termination = [string]$fault.termination
        first_fault = [string]$fault.first_fault
        last_checkpoint_present = $checkpointPresent
        last_checkpoint = if ($checkpointPresent) {
            [string]$fault.last_checkpoint
        } else {
            $null
        }
    }
}

function Assert-NoRuntimeFaultLine {
    param(
        [AllowEmptyString()][string]$Stdout,
        [AllowEmptyString()][string]$Stderr
    )
    if ([regex]::Matches(
            $Stdout,
            [regex]::Escape($script:FaultMarker)).Count -ne 0 -or
        [regex]::Matches(
            $Stderr,
            [regex]::Escape($script:FaultMarker)).Count -ne 0) {
        Throw-ProbeFailure 'unexpected-runtime-fault-line'
    }
}

function New-PrivateFaultEnvelope {
    param(
        [string]$Termination,
        [string]$FirstFault,
        [bool]$CheckpointPresent,
        [AllowNull()][string]$Checkpoint,
        [bool]$ReplayComplete
    )
    $allowedFaults = @('hang', 'guest-exception', 'dispatch-miss', 'failed')
    if ($allowedFaults -cnotcontains $Termination -or
        $allowedFaults -cnotcontains $FirstFault -or
        $Termination -cne $FirstFault -or
        ($CheckpointPresent -and [string]::IsNullOrWhiteSpace($Checkpoint)) -or
        (-not $CheckpointPresent -and $null -ne $Checkpoint)) {
        Throw-ProbeFailure 'invalid-private-fault-envelope'
    }
    return [ordered]@{
        schema = 'katana-private-runtime-fault'
        version = 1
        status = 'failed'
        termination = $Termination
        first_fault = $FirstFault
        last_checkpoint_present = $CheckpointPresent
        last_checkpoint = if ($CheckpointPresent) { $Checkpoint } else { $null }
        replay_complete = $ReplayComplete
        redacted = $true
    }
}

function Write-AtomicFaultEnvelope {
    param(
        [string]$OutputRoot,
        [ValidateSet('diagnostics-off', 'diagnostics-on', 'self-test')]
        [string]$RunLabel,
        $Envelope
    )
    if (-not (Test-Path -LiteralPath $OutputRoot -PathType Container)) {
        Throw-ProbeFailure 'fault-output-root-missing'
    }
    $outputRootFull = [IO.Path]::GetFullPath($OutputRoot)
    if (Test-PathWithinOrEqual $outputRootFull $script:RepositoryRoot) {
        Throw-ProbeFailure 'fault-output-inside-repository'
    }
    $finalPath = Join-Path $outputRootFull (
        'runtime-probe-fault-' + $RunLabel + '.json')
    if (Test-Path -LiteralPath $finalPath) {
        Throw-ProbeFailure 'fault-output-already-exists'
    }
    $temporaryPath = Join-Path $outputRootFull (
        '.runtime-probe-fault-' + [guid]::NewGuid().ToString('N') + '.tmp')
    $json = $Envelope | ConvertTo-Json -Depth 3 -Compress
    if ([string]::IsNullOrWhiteSpace($json) -or $json.Length -gt 65536) {
        Throw-ProbeFailure 'invalid-private-fault-envelope'
    }
    try {
        [IO.File]::WriteAllText(
            $temporaryPath,
            $json,
            [Text.UTF8Encoding]::new($false))
        [IO.File]::Move($temporaryPath, $finalPath)
    } catch {
        Throw-ProbeFailure 'fault-output-write-failed'
    } finally {
        if (Test-Path -LiteralPath $temporaryPath -PathType Leaf) {
            Remove-Item -LiteralPath $temporaryPath -Force
        }
    }
}

function Read-RuntimeProbeLine {
    param(
        [string]$Stdout,
        [string]$Stderr,
        [uint64]$ExpectedBudget,
        [bool]$ExpectedDiagnostics
    )
    $stdoutLines = @(Get-OutputLines $Stdout)
    $stderrLines = @(Get-OutputLines $Stderr)
    $probeLines = @($stdoutLines | Where-Object {
        $_.StartsWith($script:ProbeMarker, [StringComparison]::Ordinal)
    })
    if ($probeLines.Count -ne 1 -or
        [regex]::Matches(
            $Stdout,
            [regex]::Escape($script:ProbeMarker)).Count -ne 1 -or
        [regex]::Matches(
            $Stderr,
            [regex]::Escape($script:ProbeMarker)).Count -ne 0) {
        Throw-ProbeFailure 'runtime-probe-line-count'
    }
    $payload = $probeLines[0].Substring($script:ProbeMarker.Length)
    if ([string]::IsNullOrWhiteSpace($payload) -or
        $payload.Length -gt 1024 * 1024) {
        Throw-ProbeFailure 'invalid-runtime-probe-json'
    }
    $probe = Read-JsonObject $payload 'invalid-runtime-probe-json'
    $rootFields = @(
        'schema',
        'probe_version',
        'profile',
        'hash_contract',
        'status',
        'termination',
        'diagnostics_enabled',
        'guest_cycle_budget',
        'guest_cycle',
        'retired_guest_instructions',
        'memory_byte_count',
        'memory_range_count',
        'persistent_byte_count',
        'persistent_range_count',
        'device_count',
        'device_field_count',
        'replay',
        'hashes'
    )
    Assert-ExactFields $probe $rootFields @() 'invalid-runtime-probe-contract'
    Assert-ExactFields $probe.replay @(
        'storage_mode',
        'retention_capacity',
        'event_count',
        'retained_event_count',
        'summarized_event_count',
        'dropped_events',
        'complete',
        'exact_event_stream',
        'sealed'
    ) @() 'invalid-runtime-probe-contract'
    $hashFields = @(
        'cpu',
        'scheduler',
        'memory',
        'persistent',
        'devices',
        'guest_state',
        'replay',
        'combined'
    )
    Assert-ExactFields $probe.hashes $hashFields @() 'invalid-runtime-probe-contract'

    if ($probe.schema -isnot [string] -or
        [string]$probe.schema -cne 'katana.runtime-probe' -or
        -not (Test-IntegralJsonNumber $probe.probe_version) -or
        [decimal]$probe.probe_version -ne 3 -or
        $probe.profile -isnot [string] -or
        [string]$probe.profile -cne $script:ProbeProfile -or
        $probe.hash_contract -isnot [string] -or
        [string]$probe.hash_contract -cne 'fnv1a64-le-v1' -or
        $probe.status -isnot [string] -or
        [string]$probe.status -cne 'complete' -or
        $probe.termination -isnot [string] -or
        [string]$probe.termination -cne 'budget-reached' -or
        -not (Test-JsonBoolean $probe.diagnostics_enabled) -or
        [bool]$probe.diagnostics_enabled -ne $ExpectedDiagnostics) {
        Throw-ProbeFailure 'invalid-runtime-probe-contract'
    }

    $budget = Get-StrictUInt64 $probe.guest_cycle_budget
    $cycle = Get-StrictUInt64 $probe.guest_cycle
    $retired = Get-StrictUInt64 $probe.retired_guest_instructions
    $memoryBytes = Get-StrictUInt64 $probe.memory_byte_count
    $memoryRanges = Get-StrictUInt64 $probe.memory_range_count
    $persistentBytes = Get-StrictUInt64 $probe.persistent_byte_count
    $persistentRanges = Get-StrictUInt64 $probe.persistent_range_count
    $deviceCount = Get-StrictUInt64 $probe.device_count
    $deviceFields = Get-StrictUInt64 $probe.device_field_count
    if ($probe.replay.storage_mode -isnot [string] -or
        [string]$probe.replay.storage_mode -cne 'digest-stream') {
        Throw-ProbeFailure 'invalid-runtime-probe-contract'
    }
    $retentionCapacity = Get-StrictUInt64 $probe.replay.retention_capacity
    $eventCount = Get-StrictUInt64 $probe.replay.event_count
    $retainedEventCount = Get-StrictUInt64 $probe.replay.retained_event_count
    $summarizedEventCount = Get-StrictUInt64 $probe.replay.summarized_event_count
    $droppedEvents = Get-StrictUInt64 $probe.replay.dropped_events
    $retentionSumOverflows =
        $summarizedEventCount -gt ([uint64]::MaxValue - $retainedEventCount)
    $retentionSum = if ($retentionSumOverflows) {
        [uint64]0
    } else {
        [uint64]($retainedEventCount + $summarizedEventCount)
    }
    if ($budget -ne $ExpectedBudget -or
        $cycle -ne $ExpectedBudget -or
        $retentionCapacity -eq 0 -or
        $retentionCapacity -gt 65536 -or
        $retainedEventCount -gt $retentionCapacity -or
        $retentionSumOverflows -or
        $retentionSum -ne $eventCount -or
        ($summarizedEventCount -ne 0 -and
            $retainedEventCount -ne $retentionCapacity) -or
        $droppedEvents -ne 0 -or
        -not (Test-JsonBoolean $probe.replay.complete) -or
        -not [bool]$probe.replay.complete -or
        -not (Test-JsonBoolean $probe.replay.exact_event_stream) -or
        [bool]$probe.replay.exact_event_stream -ne
            ($summarizedEventCount -eq 0 -and $droppedEvents -eq 0) -or
        -not (Test-JsonBoolean $probe.replay.sealed) -or
        -not [bool]$probe.replay.sealed) {
        Throw-ProbeFailure 'runtime-probe-incomplete'
    }
    $normalizedHashes = [ordered]@{}
    foreach ($field in $hashFields) {
        $value = $probe.hashes.$field
        if ($value -isnot [string] -or [string]$value -cnotmatch '^[0-9a-f]{16}$') {
            Throw-ProbeFailure 'invalid-runtime-probe-hash'
        }
        $normalizedHashes[$field] = [string]$value
    }
    $normative = [ordered]@{
        schema = [string]$probe.schema
        probe_version = 3
        profile = [string]$probe.profile
        hash_contract = [string]$probe.hash_contract
        status = [string]$probe.status
        termination = [string]$probe.termination
        guest_cycle_budget = $budget.ToString([Globalization.CultureInfo]::InvariantCulture)
        guest_cycle = $cycle.ToString([Globalization.CultureInfo]::InvariantCulture)
        retired_guest_instructions =
            $retired.ToString([Globalization.CultureInfo]::InvariantCulture)
        memory_byte_count =
            $memoryBytes.ToString([Globalization.CultureInfo]::InvariantCulture)
        memory_range_count =
            $memoryRanges.ToString([Globalization.CultureInfo]::InvariantCulture)
        persistent_byte_count =
            $persistentBytes.ToString([Globalization.CultureInfo]::InvariantCulture)
        persistent_range_count =
            $persistentRanges.ToString([Globalization.CultureInfo]::InvariantCulture)
        device_count =
            $deviceCount.ToString([Globalization.CultureInfo]::InvariantCulture)
        device_field_count =
            $deviceFields.ToString([Globalization.CultureInfo]::InvariantCulture)
        replay = [ordered]@{
            storage_mode = 'digest-stream'
            retention_capacity =
                $retentionCapacity.ToString([Globalization.CultureInfo]::InvariantCulture)
            event_count =
                $eventCount.ToString([Globalization.CultureInfo]::InvariantCulture)
            retained_event_count =
                $retainedEventCount.ToString([Globalization.CultureInfo]::InvariantCulture)
            summarized_event_count =
                $summarizedEventCount.ToString([Globalization.CultureInfo]::InvariantCulture)
            dropped_events = '0'
            complete = $true
            exact_event_stream = [bool]$probe.replay.exact_event_stream
            sealed = $true
        }
        hashes = $normalizedHashes
    }
    return [pscustomobject]@{
        diagnostics_enabled = [bool]$probe.diagnostics_enabled
        normative_json = ($normative | ConvertTo-Json -Depth 5 -Compress)
    }
}

function Assert-NoWaitLoopTrace {
    param(
        [string]$Stdout,
        [string]$Stderr
    )
    $combined = $Stdout + "`n" + $Stderr
    if ($combined.IndexOf(
        'KATANA_WAIT_LOOP_TRACE',
        [StringComparison]::Ordinal) -ge 0) {
        Throw-ProbeFailure 'raw-wait-loop-trace-forbidden'
    }
    return 0
}

function Invoke-OneProbeRun {
    param(
        $RuntimeRoot,
        [string]$PackedDisc,
        [hashtable]$BaseEnvironment,
        [uint64]$GuestCycleBudget,
        [bool]$DiagnosticsEnabled,
        [int]$TimeoutSeconds,
        [string]$FaultOutputRoot,
        [ValidateSet('diagnostics-off', 'diagnostics-on')]
        [string]$RunLabel
    )
    $processResult = Invoke-BudgetedRuntime `
        ([string]$RuntimeRoot.executable) `
        @('--content', $PackedDisc) `
        ([string]$RuntimeRoot.root) `
        $BaseEnvironment `
        $GuestCycleBudget `
        $DiagnosticsEnabled `
        $TimeoutSeconds
    if ($processResult.timed_out) {
        $checkpoint = [pscustomobject]@{
            present = $false
            checkpoint = $null
            count = [uint64]0
        }
        try {
            $checkpoint = Get-RuntimeCheckpointState `
                ([string]$processResult.stdout) `
                ([string]$processResult.stderr)
        } catch {
            # A forced timeout can cut a flushed line between bytes. The host
            # timeout remains authoritative and the partial checkpoint is
            # omitted from the redacted package.
        }
        $envelope = New-PrivateFaultEnvelope `
            'hang' `
            'hang' `
            ([bool]$checkpoint.present) `
            $checkpoint.checkpoint `
            $false
        Write-AtomicFaultEnvelope $FaultOutputRoot $RunLabel $envelope
        Throw-ProbeFailure 'hang'
    }
    $checkpoint = Get-RuntimeCheckpointState `
        ([string]$processResult.stdout) `
        ([string]$processResult.stderr)
    if ($processResult.exit_code -ne 0) {
        $faultMarkerCount =
            [regex]::Matches(
                [string]$processResult.stdout,
                [regex]::Escape($script:FaultMarker)).Count +
            [regex]::Matches(
                [string]$processResult.stderr,
                [regex]::Escape($script:FaultMarker)).Count
        if ($faultMarkerCount -eq 0) {
            $envelope = New-PrivateFaultEnvelope `
                'failed' `
                'failed' `
                ([bool]$checkpoint.present) `
                $checkpoint.checkpoint `
                $false
            Write-AtomicFaultEnvelope $FaultOutputRoot $RunLabel $envelope
            Throw-ProbeFailure 'failed'
        }
        $fault = Read-RuntimeFaultLine `
            ([string]$processResult.stdout) `
            ([string]$processResult.stderr)
        if ([bool]$fault.last_checkpoint_present -ne [bool]$checkpoint.present -or
            ([bool]$checkpoint.present -and
                [string]$fault.last_checkpoint -cne
                [string]$checkpoint.checkpoint)) {
            Throw-ProbeFailure 'runtime-fault-checkpoint-mismatch'
        }
        $envelope = New-PrivateFaultEnvelope `
            ([string]$fault.termination) `
            ([string]$fault.first_fault) `
            ([bool]$fault.last_checkpoint_present) `
            $fault.last_checkpoint `
            $false
        Write-AtomicFaultEnvelope $FaultOutputRoot $RunLabel $envelope
        Throw-ProbeFailure ([string]$fault.termination)
    }
    Assert-NoRuntimeFaultLine `
        ([string]$processResult.stdout) `
        ([string]$processResult.stderr)
    if (-not [bool]$checkpoint.present) {
        Throw-ProbeFailure 'runtime-checkpoint-missing'
    }
    $probe = Read-RuntimeProbeLine `
        ([string]$processResult.stdout) `
        ([string]$processResult.stderr) `
        $GuestCycleBudget `
        $DiagnosticsEnabled
    $traceCount = Assert-NoWaitLoopTrace `
        ([string]$processResult.stdout) `
        ([string]$processResult.stderr)
    if ((Get-FileSha256 ([string]$RuntimeRoot.executable)) -cne
        [string]$RuntimeRoot.executable_sha256) {
        Throw-ProbeFailure 'runtime-executable-mutated'
    }
    return [pscustomobject]@{
        probe = $probe
        trace_count = $traceCount
        last_checkpoint_present = [bool]$checkpoint.present
        last_checkpoint = [string]$checkpoint.checkpoint
    }
}

function Test-ThrowsProbeFailure {
    param([scriptblock]$Action)
    try {
        & $Action | Out-Null
        return $false
    } catch {
        return $true
    }
}

function New-SyntheticProbeJson {
    param(
        [uint64]$Budget,
        [bool]$DiagnosticsEnabled,
        [uint64]$DroppedEvents = 0
    )
    return ([ordered]@{
        schema = 'katana.runtime-probe'
        probe_version = 3
        profile = 'deterministic-v1'
        hash_contract = 'fnv1a64-le-v1'
        status = 'complete'
        termination = 'budget-reached'
        diagnostics_enabled = $DiagnosticsEnabled
        guest_cycle_budget = $Budget
        guest_cycle = $Budget
        retired_guest_instructions = 10
        memory_byte_count = 32
        memory_range_count = 1
        persistent_byte_count = 16
        persistent_range_count = 1
        device_count = 2
        device_field_count = 3
        replay = [ordered]@{
            storage_mode = 'digest-stream'
            retention_capacity = 8
            event_count = 4
            retained_event_count = 4
            summarized_event_count = 0
            dropped_events = $DroppedEvents
            complete = $DroppedEvents -eq 0
            exact_event_stream = $DroppedEvents -eq 0
            sealed = $true
        }
        hashes = [ordered]@{
            cpu = '0000000000000001'
            scheduler = '0000000000000002'
            memory = '0000000000000003'
            persistent = '0000000000000004'
            devices = '0000000000000005'
            guest_state = '0000000000000006'
            replay = '0000000000000007'
            combined = '0000000000000008'
        }
    } | ConvertTo-Json -Depth 5 -Compress)
}

function New-SyntheticCheckpointLine {
    param(
        [uint64]$Sequence,
        [string]$Checkpoint
    )
    $json = [ordered]@{
        schema = 'katana.runtime-probe-checkpoint'
        report_version = 1
        status = 'observed'
        sequence = $Sequence
        checkpoint = $Checkpoint
    } | ConvertTo-Json -Compress
    return $script:CheckpointMarker + $json
}

function New-SyntheticFaultLine {
    param(
        [ValidateSet('hang', 'guest-exception', 'dispatch-miss', 'failed')]
        [string]$Termination,
        [AllowNull()][string]$Checkpoint
    )
    $checkpointPresent = -not [string]::IsNullOrWhiteSpace($Checkpoint)
    $json = [ordered]@{
        schema = 'katana.runtime-probe-fault'
        report_version = 1
        termination = $Termination
        first_fault_present = $true
        first_fault = $Termination
        last_checkpoint_present = $checkpointPresent
        last_checkpoint = if ($checkpointPresent) { $Checkpoint } else { $null }
    } | ConvertTo-Json -Compress
    return $script:FaultMarker + $json
}

function Invoke-PortBindingSelfTest {
    $root = Join-Path ([IO.Path]::GetTempPath()) (
        'katana-private-runtime-probe-self-test-' + [guid]::NewGuid().ToString('N'))
    $port = Join-Path $root 'port'
    $identity = 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
    $contentIdentity = 'cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc'
    $encoding = [Text.UTF8Encoding]::new($false)
    try {
        foreach ($directory in @(
            (Join-Path $port 'generated\metadata'),
            (Join-Path $port 'runtime'),
            (Join-Path $port 'content')
        )) {
            [void][IO.Directory]::CreateDirectory($directory)
        }
        $executable = Join-Path $port 'game.exe'
        [IO.File]::WriteAllText($executable, 'synthetic-executable-v1', $encoding)
        $recipePath = Join-Path $port 'content\game.katana-install'
        $recipeText = @(
            'KATANA-DISC-INSTALL 2',
            ('job ' + $identity),
            ('descriptor ' + ('b' * 64)),
            ('content ' + $contentIdentity),
            ('boot ' + ('d' * 64)),
            'tracks 1',
            ('track 1 0 4 2048 0 1 ' + ('e' * 64))
        ) -join "`n"
        [IO.File]::WriteAllText($recipePath, $recipeText + "`n", $encoding)
        $metadataText = [ordered]@{
            schema = 'katana-port-project'
            status = 'success'
            execution_profile = 'native-aot-product'
            diagnostic_partial = $false
            runtime_interpreter_enabled = $false
            project_identity = $identity
        } | ConvertTo-Json -Compress
        [IO.File]::WriteAllText(
            (Join-Path $port 'generated\metadata\port-project.json'),
            $metadataText,
            $encoding)
        $runtimeManifestText = [ordered]@{
            schema = 'katana-runtime-dependencies'
            version = 1
            linkage = 'static'
            job_generation = $identity
            files = @()
        } | ConvertTo-Json -Compress
        [IO.File]::WriteAllText(
            (Join-Path $port 'runtime\runtime-dependencies.json'),
            $runtimeManifestText,
            $encoding)
        $installManifestPath = Join-Path $port 'content\game.katana-install.json'
        $installManifest = [ordered]@{
            schema = 'katana-disc-install'
            version = 1
            job_generation = $identity
            content_identity = $contentIdentity
            artifacts = @(
                [ordered]@{
                    role = 'disc_install_recipe'
                    path = 'game.katana-install'
                    sha256 = Get-FileSha256 $recipePath
                },
                [ordered]@{
                    role = 'host_executable'
                    path = '../game.exe'
                    sha256 = Get-FileSha256 $executable
                }
            )
        }
        $validInstallManifest = $installManifest | ConvertTo-Json -Depth 4 -Compress
        [IO.File]::WriteAllText(
            $installManifestPath,
            $validInstallManifest,
            $encoding)

        $metadata = Read-PortMetadata $port
        $inventory = @(Get-RuntimeInventory $port 'game.exe' $metadata)
        $executableEntry = @($inventory | Where-Object { $_.role -ceq 'executable' })
        if ($executableEntry.Count -ne 1) {
            Throw-ProbeFailure 'self-test-port-binding-executable'
        }
        Assert-PortInstallBinding $port $metadata $executableEntry[0]

        $nonemptyRuntimeManifest = $runtimeManifestText.Replace(
            '"files":[]',
            '"files":["../game.exe"]')
        [IO.File]::WriteAllText(
            (Join-Path $port 'runtime\runtime-dependencies.json'),
            $nonemptyRuntimeManifest,
            $encoding)
        if (-not (Test-ThrowsProbeFailure {
            [void](Get-RuntimeInventory $port 'game.exe' $metadata)
        })) {
            Throw-ProbeFailure 'self-test-static-runtime-files'
        }
        [IO.File]::WriteAllText(
            (Join-Path $port 'runtime\runtime-dependencies.json'),
            $runtimeManifestText,
            $encoding)

        if ($script:IsWindowsPlatform) {
            $junctionTarget = Join-Path $root 'junction-target'
            $junction = Join-Path $port 'runtime-junction'
            [void][IO.Directory]::CreateDirectory($junctionTarget)
            [IO.File]::WriteAllText(
                (Join-Path $junctionTarget 'dependency.bin'),
                'synthetic-dependency',
                $encoding)
            try {
                New-Item -ItemType Junction -Path $junction -Target $junctionTarget |
                    Out-Null
            } catch {
                Throw-ProbeFailure 'self-test-junction-creation'
            }
            if (-not (Test-ThrowsProbeFailure {
                [void](Resolve-PhysicalPath (
                    Join-Path $junction 'dependency.bin'))
            })) {
                Throw-ProbeFailure 'self-test-reparse-component'
            }
        }

        $wrongRole = $validInstallManifest.Replace(
            '"host_executable"',
            '"host-executable"')
        [IO.File]::WriteAllText($installManifestPath, $wrongRole, $encoding)
        if (-not (Test-ThrowsProbeFailure {
            Assert-PortInstallBinding $port $metadata $executableEntry[0]
        })) {
            Throw-ProbeFailure 'self-test-port-binding-role'
        }
        [IO.File]::WriteAllText(
            $installManifestPath,
            $validInstallManifest,
            $encoding)

        [IO.File]::WriteAllText($executable, 'synthetic-executable-stale', $encoding)
        $staleInventory = @(Get-RuntimeInventory $port 'game.exe' $metadata)
        $staleExecutable = @(
            $staleInventory | Where-Object { $_.role -ceq 'executable' })
        if ($staleExecutable.Count -ne 1 -or
            -not (Test-ThrowsProbeFailure {
                Assert-PortInstallBinding $port $metadata $staleExecutable[0]
            })) {
            Throw-ProbeFailure 'self-test-port-binding-stale-executable'
        }
    } finally {
        if (Test-Path -LiteralPath $root) {
            $resolvedRoot = [IO.Path]::GetFullPath($root)
            $temporaryRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
            if (-not (Test-PathWithinOrEqual $resolvedRoot $temporaryRoot) -or
                $resolvedRoot.Equals(
                    $temporaryRoot,
                    [StringComparison]::OrdinalIgnoreCase)) {
                Throw-ProbeFailure 'self-test-cleanup-path'
            }
            Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
        }
    }
}

function Assert-SelfTestProcessTerminated {
    param([string]$PidFile)
    if (-not (Test-Path -LiteralPath $PidFile -PathType Leaf)) {
        Throw-ProbeFailure 'self-test-descendant-pid-missing'
    }
    $processId = 0
    $pidText = [IO.File]::ReadAllText($PidFile).Trim()
    if (-not [int]::TryParse(
        $pidText,
        [Globalization.NumberStyles]::None,
        [Globalization.CultureInfo]::InvariantCulture,
        [ref]$processId) -or
        $processId -le 0) {
        Throw-ProbeFailure 'self-test-descendant-pid-invalid'
    }
    $descendant = $null
    try {
        $descendant = [Diagnostics.Process]::GetProcessById($processId)
        if (-not $descendant.HasExited) {
            try { $descendant.Kill() } catch {}
            Throw-ProbeFailure 'self-test-descendant-survived'
        }
    } catch [ArgumentException] {
        # Expected: the KILL_ON_JOB_CLOSE job already removed the descendant.
    } finally {
        if ($null -ne $descendant) { $descendant.Dispose() }
    }
}

function New-SelfTestDescendantCommand {
    param(
        [string]$PidFile,
        [bool]$RootExits
    )
    $hostPath = [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    $hostBase64 =
        [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($hostPath))
    $pidBase64 =
        [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($PidFile))
    $grandchildCommand = 'Start-Sleep -Seconds 30'
    $grandchildEncoded = [Convert]::ToBase64String(
        [Text.Encoding]::Unicode.GetBytes($grandchildCommand))
    $tail = if ($RootExits) { 'exit 0' } else { 'Start-Sleep -Seconds 30' }
    return @"
`$hostPath = [Text.Encoding]::UTF8.GetString(
    [Convert]::FromBase64String('$hostBase64'))
`$pidFile = [Text.Encoding]::UTF8.GetString(
    [Convert]::FromBase64String('$pidBase64'))
`$child = Start-Process -FilePath `$hostPath -ArgumentList (
    '-NoProfile -NonInteractive -EncodedCommand $grandchildEncoded') -PassThru -WindowStyle Hidden
[IO.File]::WriteAllText(
    `$pidFile,
    `$child.Id.ToString([Globalization.CultureInfo]::InvariantCulture))
$tail
"@
}

function Invoke-JobAndCaptureSelfTest {
    param([hashtable]$BaseEnvironment)
    $hostExecutable = [Diagnostics.Process]::GetCurrentProcess().MainModule.FileName
    $timeoutPid = Join-Path ([IO.Path]::GetTempPath()) (
        'katana-runtime-probe-timeout-' + [guid]::NewGuid().ToString('N') + '.pid')
    $exitPid = Join-Path ([IO.Path]::GetTempPath()) (
        'katana-runtime-probe-exit-' + [guid]::NewGuid().ToString('N') + '.pid')
    try {
        $timeoutCommand = New-SelfTestDescendantCommand $timeoutPid $false
        $timeoutResult = Invoke-BudgetedRuntime `
            $hostExecutable `
            @('-NoProfile', '-NonInteractive', '-Command', $timeoutCommand) `
            $script:RepositoryRoot `
            $BaseEnvironment `
            1 `
            $false `
            2 `
            4096
        if (-not $timeoutResult.timed_out -or
            $timeoutResult.elapsed_milliseconds -gt 12000) {
            Throw-ProbeFailure 'self-test-timeout'
        }
        Assert-SelfTestProcessTerminated $timeoutPid

        $exitCommand = New-SelfTestDescendantCommand $exitPid $true
        $exitResult = Invoke-BudgetedRuntime `
            $hostExecutable `
            @('-NoProfile', '-NonInteractive', '-Command', $exitCommand) `
            $script:RepositoryRoot `
            $BaseEnvironment `
            1 `
            $false `
            5 `
            4096
        if ($exitResult.timed_out -or $exitResult.exit_code -ne 0) {
            Throw-ProbeFailure 'self-test-root-exit'
        }
        Assert-SelfTestProcessTerminated $exitPid

        $dualStreamCommand =
            "[Console]::Out.Write(('o' * 2048)); " +
            "[Console]::Error.Write(('e' * 2048)); exit 0"
        $dualStream = Invoke-BudgetedRuntime `
            $hostExecutable `
            @('-NoProfile', '-NonInteractive', '-Command', $dualStreamCommand) `
            $script:RepositoryRoot `
            $BaseEnvironment `
            1 `
            $false `
            5 `
            4096
        if ($dualStream.timed_out -or $dualStream.exit_code -ne 0 -or
            $dualStream.stdout.Length -lt 2048 -or
            $dualStream.stderr.Length -lt 2048) {
            Throw-ProbeFailure 'self-test-concurrent-capture'
        }

        $overLimitCommand =
            "[Console]::Out.Write(('x' * 8192)); Start-Sleep -Seconds 30"
        if (-not (Test-ThrowsProbeFailure {
            [void](Invoke-BudgetedRuntime `
                $hostExecutable `
                @('-NoProfile', '-NonInteractive', '-Command', $overLimitCommand) `
                $script:RepositoryRoot `
                $BaseEnvironment `
                1 `
                $false `
                5 `
                4096)
        })) {
            Throw-ProbeFailure 'self-test-capture-limit'
        }
    } finally {
        foreach ($pidFile in @($timeoutPid, $exitPid)) {
            if (Test-Path -LiteralPath $pidFile -PathType Leaf) {
                Remove-Item -LiteralPath $pidFile -Force
            }
        }
    }
}

function Invoke-SelfTest {
    $validConfig = @'
{
  "port_root": "private-port",
  "executable_relative": "game.exe",
  "packed_disc_path": "private-pack.katana-disc",
  "output_root": "private-output",
  "host_timeout_seconds": 1,
  "guest_cycle_budget": 100
}
'@
    $parsed = ConvertFrom-ProbeConfig $validConfig
    if ($parsed.host_timeout_seconds -ne 1 -or
        $parsed.guest_cycle_budget -ne 100) {
        Throw-ProbeFailure 'self-test-config-valid'
    }
    foreach ($invalid in @(
        $validConfig.Replace('"host_timeout_seconds": 1', '"host_timeout_seconds": 0'),
        $validConfig.Replace('"host_timeout_seconds": 1', '"host_timeout_seconds": 901'),
        $validConfig.Replace('"host_timeout_seconds": 1', '"host_timeout_seconds": 1.0'),
        $validConfig.Replace('"guest_cycle_budget": 100', '"guest_cycle_budget": 0'),
        $validConfig.Replace('"guest_cycle_budget": 100', '"guest_cycle_budget": 1.0'),
        $validConfig.Replace(
            '"guest_cycle_budget": 100',
            '"guest_cycle_budget": 100, "unknown": true'),
        $validConfig.Replace(
            '"port_root": "private-port"',
            '"port_root": "private-port", "port_root": "shadow-port"')
    )) {
        if (-not (Test-ThrowsProbeFailure {
            [void](ConvertFrom-ProbeConfig $invalid)
        })) {
            Throw-ProbeFailure 'self-test-config-invalid'
        }
    }

    $probeOff = New-SyntheticProbeJson 100 $false
    $parsedProbe = Read-RuntimeProbeLine `
        ("noise`n" + $script:ProbeMarker + $probeOff + "`n") '' 100 $false
    if ($parsedProbe.diagnostics_enabled -or
        [string]::IsNullOrWhiteSpace($parsedProbe.normative_json)) {
        Throw-ProbeFailure 'self-test-probe-valid'
    }
    foreach ($invalidProbe in @(
        ($script:ProbeMarker + $probeOff + "`n" + $script:ProbeMarker + $probeOff),
        ($script:ProbeMarker + (New-SyntheticProbeJson 101 $false)),
        ($script:ProbeMarker + (New-SyntheticProbeJson 100 $true)),
        ($script:ProbeMarker + (New-SyntheticProbeJson 100 $false 1)),
        ($script:ProbeMarker + $probeOff.Replace(
            '"probe_version":3',
            '"probe_version":3,"probe_version":3'))
    )) {
        if (-not (Test-ThrowsProbeFailure {
            [void](Read-RuntimeProbeLine $invalidProbe '' 100 $false)
        })) {
            Throw-ProbeFailure 'self-test-probe-invalid'
        }
    }

    $checkpointOutput =
        (New-SyntheticCheckpointLine 1 'runtime-started') + "`n" +
        (New-SyntheticCheckpointLine 2 'guest-program-entered') + "`n"
    $checkpointState = Get-RuntimeCheckpointState $checkpointOutput ''
    if (-not $checkpointState.present -or
        $checkpointState.count -ne 2 -or
        [string]$checkpointState.checkpoint -cne 'guest-program-entered') {
        Throw-ProbeFailure 'self-test-checkpoint-valid'
    }
    foreach ($invalidCheckpoint in @(
        (New-SyntheticCheckpointLine 2 'runtime-started'),
        ((New-SyntheticCheckpointLine 1 'guest-program-entered') + "`n" +
            (New-SyntheticCheckpointLine 2 'runtime-started')),
        ((New-SyntheticCheckpointLine 1 'runtime-started').Replace(
            '"status":"observed"',
            '"status":"observed","address":"private-value"'))
    )) {
        if (-not (Test-ThrowsProbeFailure {
            [void](Get-RuntimeCheckpointState $invalidCheckpoint '')
        })) {
            Throw-ProbeFailure 'self-test-checkpoint-invalid'
        }
    }

    $guestFaultLine =
        New-SyntheticFaultLine 'guest-exception' 'guest-program-entered'
    $guestFault = Read-RuntimeFaultLine $guestFaultLine ''
    if ([string]$guestFault.termination -cne 'guest-exception' -or
        -not $guestFault.last_checkpoint_present -or
        [string]$guestFault.last_checkpoint -cne 'guest-program-entered') {
        Throw-ProbeFailure 'self-test-guest-fault-valid'
    }
    $dispatchFault = Read-RuntimeFaultLine (
        New-SyntheticFaultLine 'dispatch-miss' $null) ''
    if ([string]$dispatchFault.termination -cne 'dispatch-miss' -or
        $dispatchFault.last_checkpoint_present) {
        Throw-ProbeFailure 'self-test-dispatch-fault-valid'
    }
    foreach ($invalidFault in @(
        ($guestFaultLine + "`n" + $guestFaultLine),
        $guestFaultLine.Replace(
            '"termination":"guest-exception"',
            '"termination":"guest-exception","address":"8c010000"'),
        $guestFaultLine.Replace(
            '"report_version":1',
            '"report_version":1,"report_version":1'),
        $guestFaultLine.Replace(
            '"first_fault":"guest-exception"',
            '"first_fault":"dispatch-miss"')
    )) {
        if (-not (Test-ThrowsProbeFailure {
            [void](Read-RuntimeFaultLine $invalidFault '')
        })) {
            Throw-ProbeFailure 'self-test-fault-invalid'
        }
    }

    $faultOutputRoot = Join-Path ([IO.Path]::GetTempPath()) (
        'katana-runtime-probe-fault-self-test-' +
        [guid]::NewGuid().ToString('N'))
    try {
        [void][IO.Directory]::CreateDirectory($faultOutputRoot)
        $hangEnvelope = New-PrivateFaultEnvelope `
            'hang' 'hang' $true 'runtime-started' $false
        Write-AtomicFaultEnvelope $faultOutputRoot 'self-test' $hangEnvelope
        $faultPath = Join-Path $faultOutputRoot (
            'runtime-probe-fault-self-test.json')
        Assert-RegularNonReparseFile $faultPath 'self-test-fault-output'
        $faultText = [IO.File]::ReadAllText($faultPath)
        $faultObject = Read-JsonObject $faultText 'self-test-fault-output'
        Assert-ExactFields $faultObject @(
            'schema',
            'version',
            'status',
            'termination',
            'first_fault',
            'last_checkpoint_present',
            'last_checkpoint',
            'replay_complete',
            'redacted'
        ) @() 'self-test-fault-output'
        if ([string]$faultObject.schema -cne 'katana-private-runtime-fault' -or
            [string]$faultObject.termination -cne 'hang' -or
            [string]$faultObject.first_fault -cne 'hang' -or
            [string]$faultObject.last_checkpoint -cne 'runtime-started' -or
            [bool]$faultObject.replay_complete -or
            -not [bool]$faultObject.redacted -or
            $faultText -match '(?i)address|register|hash|path|stdout|stderr|8c[0-9a-f]{6}') {
            Throw-ProbeFailure 'self-test-fault-output'
        }
        if (-not (Test-ThrowsProbeFailure {
            Write-AtomicFaultEnvelope $faultOutputRoot 'self-test' $hangEnvelope
        })) {
            Throw-ProbeFailure 'self-test-fault-atomic-replace'
        }
    } finally {
        if (Test-Path -LiteralPath $faultOutputRoot -PathType Container) {
            Remove-Item -LiteralPath $faultOutputRoot -Recurse -Force
        }
    }

    if ((Assert-NoWaitLoopTrace '' '') -ne 0) {
        Throw-ProbeFailure 'self-test-trace-off'
    }
    if (-not (Test-ThrowsProbeFailure {
        [void](Assert-NoWaitLoopTrace '' $script:TraceNotice)
    })) {
        Throw-ProbeFailure 'self-test-trace-leak'
    }
    Invoke-PortBindingSelfTest

    if ((ConvertTo-CommandLineArgument '') -cne '""' -or
        (ConvertTo-CommandLineArgument 'simple') -cne 'simple' -or
        (ConvertTo-CommandLineArgument 'with space') -cne '"with space"') {
        Throw-ProbeFailure 'self-test-command-line-quoting'
    }
    $baseEnvironment = Get-SanitizedBaseEnvironment
    Invoke-JobAndCaptureSelfTest $baseEnvironment
}

function Invoke-ConfiguredProbe {
    param([string]$ConfigArgument)
    if ([string]::IsNullOrWhiteSpace($ConfigArgument)) {
        Throw-ProbeFailure 'config-required'
    }
    $configPath = Assert-OutsideRepository $ConfigArgument $script:RepositoryRoot
    Assert-RegularNonReparseFile $configPath 'invalid-config-path'
    try {
        $configText = Get-Content -LiteralPath $configPath -Raw
    } catch {
        Throw-ProbeFailure 'invalid-config-path'
    }
    $settings = ConvertFrom-ProbeConfig $configText
    $configDirectory = Split-Path -Parent $configPath
    $portRoot = Assert-OutsideRepository `
        (Resolve-ConfigPath $settings.port_root $configDirectory) `
        $script:RepositoryRoot
    $packedDisc = Assert-OutsideRepository `
        (Resolve-ConfigPath $settings.packed_disc_path $configDirectory) `
        $script:RepositoryRoot
    $outputRoot = Assert-OutsideRepository `
        (Resolve-ConfigPath $settings.output_root $configDirectory) `
        $script:RepositoryRoot
    if (-not (Test-Path -LiteralPath $portRoot -PathType Container)) {
        Throw-ProbeFailure 'private-port-missing'
    }
    Assert-RegularNonReparseFile $packedDisc 'private-packed-disc-missing'
    if (Test-Path -LiteralPath $outputRoot -PathType Leaf) {
        Throw-ProbeFailure 'invalid-output-root'
    }
    if ((Test-PathWithinOrEqual $outputRoot $portRoot) -or
        (Test-PathWithinOrEqual $portRoot $outputRoot)) {
        Throw-ProbeFailure 'output-root-overlaps-port'
    }
    [void][IO.Directory]::CreateDirectory($outputRoot)
    $outputRoot = Resolve-PhysicalPath $outputRoot

    $metadata = Read-PortMetadata $portRoot
    $inventory = @(
        Get-RuntimeInventory $portRoot $settings.executable_relative $metadata
    )
    if ($inventory.Count -lt 2) {
        Throw-ProbeFailure 'runtime-inventory-incomplete'
    }
    $executableEntry = @($inventory | Where-Object { $_.role -ceq 'executable' })
    if ($executableEntry.Count -ne 1) {
        Throw-ProbeFailure 'runtime-inventory-executable-count'
    }
    Assert-PortInstallBinding $portRoot $metadata $executableEntry[0]
    $packSha256 = Get-FileSha256 $packedDisc
    Assert-InventoryUnchanged $inventory

    $runtimeA = New-FreshRuntimeRoot `
        $outputRoot $inventory $settings.executable_relative 'diagnostics-off'
    $runtimeB = New-FreshRuntimeRoot `
        $outputRoot $inventory $settings.executable_relative 'diagnostics-on'
    if ([string]$runtimeA.root -ceq [string]$runtimeB.root -or
        [string]$runtimeA.executable_sha256 -cne [string]$executableEntry[0].sha256 -or
        [string]$runtimeB.executable_sha256 -cne [string]$executableEntry[0].sha256) {
        Throw-ProbeFailure 'runtime-roots-not-identical-and-unique'
    }

    $baseEnvironment = Get-SanitizedBaseEnvironment
    $runA = Invoke-OneProbeRun `
        $runtimeA `
        $packedDisc `
        $baseEnvironment `
        $settings.guest_cycle_budget `
        $false `
        $settings.host_timeout_seconds `
        $outputRoot `
        'diagnostics-off'
    if ((Get-FileSha256 $packedDisc) -cne $packSha256) {
        Throw-ProbeFailure 'packed-disc-changed'
    }
    Assert-InventoryUnchanged $inventory
    $runB = Invoke-OneProbeRun `
        $runtimeB `
        $packedDisc `
        $baseEnvironment `
        $settings.guest_cycle_budget `
        $true `
        $settings.host_timeout_seconds `
        $outputRoot `
        'diagnostics-on'
    if ((Get-FileSha256 $packedDisc) -cne $packSha256) {
        Throw-ProbeFailure 'packed-disc-changed'
    }
    Assert-InventoryUnchanged $inventory

    if ([string]$runA.probe.normative_json -cne
        [string]$runB.probe.normative_json) {
        Throw-ProbeFailure 'normative-runtime-probe-mismatch'
    }
    if ([bool]$runA.last_checkpoint_present -ne
            [bool]$runB.last_checkpoint_present -or
        ([bool]$runA.last_checkpoint_present -and
            [string]$runA.last_checkpoint -cne
            [string]$runB.last_checkpoint)) {
        Throw-ProbeFailure 'runtime-checkpoint-ab-mismatch'
    }
    if ($runA.probe.diagnostics_enabled -or
        -not $runB.probe.diagnostics_enabled -or
        $runA.trace_count -ne 0 -or
        $runB.trace_count -ne 0) {
        Throw-ProbeFailure 'diagnostics-ab-contract-mismatch'
    }
    return [ordered]@{
        schema = 'katana-private-runtime-probe-ab'
        version = 1
        config_version = 1
        status = 'success'
        profile = $script:ProbeProfile
        runs = 2
        guest_cycle_budget = $settings.guest_cycle_budget
        host_timeout_seconds = $settings.host_timeout_seconds
        diagnostics = [ordered]@{
            off = $false
            on = $true
        }
        normative_fields_equal = $true
        last_checkpoint_equal = $true
        executable_and_pack_unchanged = $true
        replay_complete_and_sealed = $true
        trace_lines = [ordered]@{
            diagnostics_off = 0
            diagnostics_on = 0
        }
    }
}

try {
    if ($SelfTest) {
        Invoke-SelfTest
        $selfReport = [ordered]@{
            schema = 'katana-private-runtime-probe-self-test'
            version = 1
            status = 'success'
        }
        Write-Output ($selfReport | ConvertTo-Json -Compress)
        exit 0
    }
    $successReport = Invoke-ConfiguredProbe $Config
    Write-Output ($successReport | ConvertTo-Json -Depth 4 -Compress)
    exit 0
} catch {
    $failureReport = [ordered]@{
        schema = 'katana-private-runtime-probe-ab'
        version = 1
        status = 'failed'
        failure_class = Get-ProbeFailureClass $_.Exception
    }
    Write-Output ($failureReport | ConvertTo-Json -Compress)
    exit 5
}
