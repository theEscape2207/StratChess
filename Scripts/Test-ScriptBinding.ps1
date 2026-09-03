<#
.SYNOPSIS
    Fail when a script with a `param()` block does not declare `[CmdletBinding()]`.

.DESCRIPTION
    Without `[CmdletBinding()]` PowerShell binds a script's parameters loosely: an
    argument it does not recognise is discarded in silence and the body runs with
    defaults. For a script whose defaults are cheap that is untidy. For one whose
    defaults start work it is a trap.

    `Run-EloMatch.ps1` was the trap. `pwsh -File Scripts\Run-EloMatch.ps1 -?`
    printed no help: `-?` was swallowed and the script ran its default 500-game
    anchor match, which then contended for the box with an SPRT already running and
    invalidated it. That happened twice, months apart, and cost hours of wall-clock
    both times. `Run-Bench.ps1`, which does declare `[CmdletBinding()]`, prints help
    for the same argument and does nothing else -- the attribute is the whole
    difference.

    So the rule is not "support -?". It is that every parameterised script rejects
    what it does not understand, which fixes `-?` and every mistyped flag at once.

    Scripts with no `param()` block are exempt: there is nothing to bind, and
    PowerShell has no arguments to mis-bind.

    Detection is by AST, not by regex over the text -- `[CmdletBinding()]` inside a
    comment, a here-string or a nested function must not satisfy the check, and only
    the parser can tell those apart from the real attribute on the script's own
    param block.

.PARAMETER Root
    Repository to check. Every tracked `.ps1` under it is checked, wherever it lives
    -- `build.ps1` sits at the root, not in `Scripts/`. Defaults to the repository
    containing this script, so it is correct in every worktree.

.PARAMETER SelfTest
    Run synthetic parser cases and exit. Verifies the detector actually detects,
    which a green run over already-compliant scripts does not.

.HOW TO INVOKE
    pwsh -File Scripts/Test-ScriptBinding.ps1
    pwsh -File Scripts/Test-ScriptBinding.ps1 -SelfTest
#>

[CmdletBinding(DefaultParameterSetName = 'Run')]
param(
    [Parameter(ParameterSetName = 'Run')]
    [string]$Root,

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Test-ScriptText {
    <#
      .SYNOPSIS
        Classify one script's text: 'ok', 'no-param' or 'unbound'. Throws if the
        text does not parse, because an unparseable script is not a pass.
    #>
    param(
        [Parameter(Mandatory)][AllowEmptyString()][string]$Text,
        [Parameter(Mandatory)][string]$Label
    )

    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseInput($Text, [ref]$tokens, [ref]$errors)
    if ($errors.Count -gt 0) {
        throw "${Label}: does not parse ($($errors[0].Message))"
    }

    # ParamBlock is the script's OWN param() block. A param() inside a function
    # hangs off that function's ast instead, so it cannot satisfy this.
    $paramBlock = $ast.ParamBlock
    if ($null -eq $paramBlock) { return 'no-param' }

    foreach ($attribute in $paramBlock.Attributes) {
        # Compared by name rather than by reflected type: an attribute the session
        # cannot resolve reflects as $null, and a script is not exempt just because
        # this checker could not load one of its attribute types.
        if ($attribute.TypeName.Name -in @('CmdletBinding', 'CmdletBindingAttribute')) {
            return 'ok'
        }
    }
    return 'unbound'
}

function Invoke-SelfTest {
    function Assert-Case {
        param(
            [Parameter(Mandatory)][string]$Name,
            [Parameter(Mandatory)][AllowEmptyString()][string]$Content,
            [string]$Expected,
            [switch]$ExpectThrow
        )

        try {
            $actual = Test-ScriptText -Text $Content -Label 'self-test'
        }
        catch {
            if ($ExpectThrow) {
                Write-Host "  PASS  $Name (rejected: $($_.Exception.Message))" -ForegroundColor Green
            }
            else {
                Write-Host "  FAIL  $Name (unexpected error: $($_.Exception.Message))" -ForegroundColor Red
                $script:selfTestFailures++
            }
            return
        }

        if ($ExpectThrow) {
            Write-Host "  FAIL  $Name (expected an error, got '$actual')" -ForegroundColor Red
            $script:selfTestFailures++
            return
        }

        if ($actual -eq $Expected) {
            Write-Host "  PASS  $Name" -ForegroundColor Green
        }
        else {
            Write-Host "  FAIL  $Name (expected '$Expected', got '$actual')" -ForegroundColor Red
            $script:selfTestFailures++
        }
    }

    $script:selfTestFailures = 0
    Write-Host "==> Self-test" -ForegroundColor Cyan

    Assert-Case -Name 'bound script passes' -Expected 'ok' -Content @'
[CmdletBinding()]
param([string]$Foo = '')
Write-Host $Foo
'@

    # The one that matters: this is Run-EloMatch.ps1's shape before the fix.
    Assert-Case -Name 'unbound script is caught' -Expected 'unbound' -Content @'
param([string]$Foo = '')
Write-Host $Foo
'@

    Assert-Case -Name 'no param block is exempt' -Expected 'no-param' -Content @'
Write-Host 'hello'
'@

    Assert-Case -Name 'CmdletBinding with arguments still counts' -Expected 'ok' -Content @'
[CmdletBinding(DefaultParameterSetName = 'Run')]
param([Parameter(ParameterSetName = 'Run')][string]$Foo = '')
'@

    Assert-Case -Name 'other attributes do not count' -Expected 'unbound' -Content @'
[OutputType([string])]
param([string]$Foo = '')
'@

    # The three ways a regex over the text would be fooled.
    Assert-Case -Name 'CmdletBinding in a comment does not count' -Expected 'unbound' -Content @'
# [CmdletBinding()]
param([string]$Foo = '')
'@

    # Assembled from parts rather than written as a here-string: the case's own
    # content contains a here-string terminator, which would close this one early.
    Assert-Case -Name 'CmdletBinding in a here-string does not count' -Expected 'unbound' -Content (@(
            'param([string]$Foo = '''')'
            '$sample = @'''
            '[CmdletBinding()]'
            '''@'
        ) -join "`n")

    Assert-Case -Name 'CmdletBinding on a nested function does not count' -Expected 'unbound' -Content @'
param([string]$Foo = '')
function Inner {
    [CmdletBinding()]
    param([string]$Bar)
}
'@

    Assert-Case -Name 'a script that does not parse is an error' -ExpectThrow -Content @'
param([string]$Foo = ''
'@

    $failures = $script:selfTestFailures
    if ($failures -gt 0) {
        Write-Host "$failures self-test case(s) FAILED." -ForegroundColor Red
        return $false
    }
    Write-Host "Self-test PASSED." -ForegroundColor Green
    return $true
}

if ($SelfTest) {
    if (Invoke-SelfTest) { exit 0 }
    exit 1
}

if (-not $Root) { $Root = Split-Path -Parent $PSScriptRoot }

if (-not (Test-Path -LiteralPath $Root -PathType Container)) {
    Write-Host "FAIL: no directory at $Root" -ForegroundColor Red
    exit 1
}

# Tracked files rather than a directory walk, for both halves of the scope: the
# scripts are in two places (`Scripts/` and `build.ps1` at the root), and a walk
# would also reach the .ps1 files that FetchContent and tool downloads leave under
# build/, which are not ours to fix.
$scripts = @(& git -C $Root ls-files '*.ps1' | Where-Object { $_ } | Sort-Object)
if ($scripts.Count -eq 0) {
    Write-Host "FAIL: git listed no tracked .ps1 files under $Root" -ForegroundColor Red
    exit 1
}

Write-Host "==> Script parameter binding ($($scripts.Count) script(s))" -ForegroundColor Cyan

$violations = 0
$exempt = 0
foreach ($script in $scripts) {
    $text = Get-Content -LiteralPath (Join-Path $Root $script) -Raw
    try {
        $verdict = Test-ScriptText -Text $text -Label $script
    }
    catch {
        Write-Host "  FAIL  ${script}: $($_.Exception.Message)" -ForegroundColor Red
        $violations++
        continue
    }

    switch ($verdict) {
        'ok' { }
        'no-param' { $exempt++ }
        'unbound' {
            Write-Host "  FAIL  ${script}: param() block without [CmdletBinding()]" -ForegroundColor Red
            $violations++
        }
    }
}

if ($violations -gt 0) {
    Write-Host ""
    Write-Host "$violations script(s) bind parameters loosely. Add [CmdletBinding()] above param()." -ForegroundColor Red
    Write-Host "Without it an unrecognised argument is discarded silently and the script runs its" -ForegroundColor Yellow
    Write-Host "defaults -- which is how 'Run-EloMatch.ps1 -?' twice started a 500-game match." -ForegroundColor Yellow
    exit 1
}

Write-Host "PASS: every parameterised script declares [CmdletBinding()] ($exempt with no parameters)." -ForegroundColor Green
exit 0
