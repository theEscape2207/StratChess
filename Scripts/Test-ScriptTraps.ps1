<#
.SYNOPSIS
    Fail on two PowerShell traps that fail silently: a script whose param() block
    lacks [CmdletBinding()], and one variable spelled two ways in one scope.

.DESCRIPTION
    Binding. Without `[CmdletBinding()]` PowerShell binds a script's parameters
    loosely: an argument it does not recognise is discarded in silence and the body
    runs with defaults. For a script whose defaults start work that is a trap.
    `pwsh -File Scripts\Run-EloMatch.ps1 -?` printed no help: `-?` was swallowed and
    the script ran its default 500-game anchor match, which contended for the box
    with an SPRT already running and invalidated it. That happened twice, months
    apart. So the rule is not "support -?": every parameterised script rejects what
    it does not understand, which fixes `-?` and every mistyped flag at once.
    Scripts with no `param()` block are exempt: there is nothing to bind.

    Casing. Variable names are case-insensitive, so `$mainCheckout` and
    `$MainCheckout` are one variable. In the same scope the second assignment
    overwrites the first; inside a function it shadows the outer one for every read
    there. Both shipped: `Get-Worktrees.ps1` ran `git -C False`, and #387 made
    whole-tree lint unconditional. The check groups each scope's variables by
    lower-cased name and fails on any group spelled more than one way. A function
    local spelled exactly like an outer variable still shadows it, and nothing here
    can see that: write-powershell rule 2 covers it.

    Detection is by AST, not by regex over the text -- an attribute or a variable
    inside a comment or a here-string must not count, and only the parser can tell.

.PARAMETER Root
    Repository to check in whole-tree mode. Every tracked `.ps1` under it is
    checked, wherever it lives -- `build.ps1` sits at the root, not in `Scripts/`.
    Defaults to the repository containing this script.

.PARAMETER BaseRef
    Check only the `.ps1` files changed since this ref, as Get-ChangeTier.ps1 reports
    them. A change to this script runs its self-test and checks the whole tree, so a
    new rule holds everywhere from the PR that adds it; a diff that cannot be
    computed checks the whole tree too. The
    nightly whole-tree run catches two PRs that are clean apart and clash once merged.

.PARAMETER SelfTest
    Run synthetic parser cases and exit. Verifies the detectors actually detect,
    which a green run over already-compliant scripts does not.

.HOW TO INVOKE
    pwsh -File Scripts/Test-ScriptTraps.ps1                       # whole tree
    pwsh -File Scripts/Test-ScriptTraps.ps1 -BaseRef origin/main  # changed scripts
    pwsh -File Scripts/Test-ScriptTraps.ps1 -SelfTest
#>

[CmdletBinding(DefaultParameterSetName = 'Tree')]
param(
    [Parameter(ParameterSetName = 'Tree')]
    [string]$Root,

    [Parameter(Mandatory, ParameterSetName = 'Changed')]
    [string]$BaseRef,

    [Parameter(Mandatory, ParameterSetName = 'SelfTest')]
    [switch]$SelfTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Read-ScriptAst {
    <#
      .SYNOPSIS
        Parse one script's text. Throws if it does not parse, because an
        unparseable script is not a pass.
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
    return $ast
}

function Get-BindingVerdict {
    <#
      .SYNOPSIS
        'ok', 'no-param' or 'unbound' for a script's own param() block.
    #>
    param([Parameter(Mandatory)][System.Management.Automation.Language.ScriptBlockAst]$Ast)

    # ParamBlock is the script's OWN param() block. A param() inside a function
    # hangs off that function's ast instead, so it cannot satisfy this.
    $paramBlock = $Ast.ParamBlock
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

function Get-CasingClash {
    <#
      .SYNOPSIS
        One line per variable spelled more than one way within one scope, e.g.
        "<script>: $mainCheckout (L234), $MainCheckout (L72)". Empty when clean.
    #>
    param([Parameter(Mandatory)][System.Management.Automation.Language.ScriptBlockAst]$Ast)

    $variables = $Ast.FindAll(
        { $args[0] -is [System.Management.Automation.Language.VariableExpressionAst] }, $true) |
        Where-Object {
            # `$env:PATH` is an environment lookup, and `$True`/`$true` are constants
            # that cannot be overwritten: a second spelling of either is cosmetic.
            -not $_.VariablePath.IsDriveQualified -and
            $_.VariablePath.UserPath -notin @('true', 'false', 'null')
        }

    $clashes = foreach ($group in ($variables | Group-Object {
                $scope = $_.Parent
                while ($scope -and $scope -isnot [System.Management.Automation.Language.FunctionDefinitionAst]) {
                    $scope = $scope.Parent
                }
                $scopeName = if ($scope) { $scope.Name } else { '<script>' }
                # `$script:Foo` and `$foo` are the same variable at script scope.
                $name = $_.VariablePath.UserPath -replace '^(script|local|global|private):', ''
                "$scopeName|$($name.ToLowerInvariant())"
            })) {
        $spellings = @($group.Group | Group-Object { $_.VariablePath.UserPath -replace '^(script|local|global|private):', '' } `
                -CaseSensitive)
        if ($spellings.Count -lt 2) { continue }

        $scopeName = $group.Name.Split('|')[0]
        $detail = ($spellings | ForEach-Object {
                '${0} (L{1})' -f $_.Name, $_.Group[0].Extent.StartLineNumber
            }) -join ', '
        "${scopeName}: $detail"
    }
    return @($clashes)
}

function Invoke-SelfTest {
    $failures = 0
    Write-Host "==> Self-test" -ForegroundColor Cyan

    # Binding: the one that matters is Run-EloMatch.ps1's shape before its fix.
    $bindingCases = @(
        @{ Name = 'bound script passes'; Expect = 'ok'; Text = "[CmdletBinding()]`nparam([string]`$Foo = '')" }
        @{ Name = 'unbound script is caught'; Expect = 'unbound'; Text = "param([string]`$Foo = '')" }
        @{ Name = 'no param block is exempt'; Expect = 'no-param'; Text = "Write-Host 'hello'" }
        @{ Name = 'CmdletBinding with arguments counts'; Expect = 'ok'
            Text = "[CmdletBinding(DefaultParameterSetName = 'Run')]`nparam([Parameter(ParameterSetName = 'Run')][string]`$Foo)" }
        @{ Name = 'other attributes do not count'; Expect = 'unbound'; Text = "[OutputType([string])]`nparam([string]`$Foo)" }
        # The three ways a regex over the text would be fooled.
        @{ Name = 'CmdletBinding in a comment does not count'; Expect = 'unbound'; Text = "# [CmdletBinding()]`nparam([string]`$Foo)" }
        @{ Name = 'CmdletBinding in a here-string does not count'; Expect = 'unbound'
            Text = "param([string]`$Foo)`n`$sample = @'`n[CmdletBinding()]`n'@" }
        @{ Name = 'CmdletBinding on a nested function does not count'; Expect = 'unbound'
            Text = "param([string]`$Foo)`nfunction Inner {`n    [CmdletBinding()]`n    param([string]`$Bar)`n}" }
    )

    # Casing: Expect is the number of clashes reported.
    $casingCases = @(
        # Get-Worktrees.ps1's shape before its fix: a same-scope overwrite.
        @{ Name = 'same-scope overwrite is caught'; Expect = 1
            Text = "`$MainCheckout = 'C:\repo'`nforeach (`$e in 1) { `$mainCheckout = `$true }`ngit -C `$MainCheckout status" }
        # #387's shape: a function local shadowing the script parameter.
        @{ Name = 'function local shadowing a parameter is caught'; Expect = 1
            Text = "param([switch]`$All)`nfunction Get-Files {`n    `$all = @('a')`n    if (`$All) { `$all }`n}" }
        @{ Name = 'script: qualifier is the same variable'; Expect = 1
            Text = "`$script:Count = 0`n`$count = 1" }
        @{ Name = 'one spelling reused passes'; Expect = 0; Text = "`$files = 1`n`$files = `$files + 1" }
        @{ Name = 'one name in separate functions passes'; Expect = 0
            Text = "function A { `$files = 1 }`nfunction B { param(`$Files) `$Files }" }
        @{ Name = 'distinct names pass'; Expect = 0; Text = "`$MainCheckout = 'x'`n`$isMainCheckout = `$true" }
        @{ Name = 'env and constants are exempt'; Expect = 0; Text = "`$env:PATH`n`$env:Path`n`$True`n`$true" }
        @{ Name = 'a spelling in a comment does not count'; Expect = 0; Text = "`$Foo = 1`n# `$foo" }
    )

    foreach ($case in $bindingCases) {
        $actual = Get-BindingVerdict -Ast (Read-ScriptAst -Text $case.Text -Label $case.Name)
        if ($actual -eq $case.Expect) { Write-Host "  PASS  $($case.Name)" -ForegroundColor Green }
        else {
            Write-Host "  FAIL  $($case.Name) (expected '$($case.Expect)', got '$actual')" -ForegroundColor Red
            $failures++
        }
    }

    foreach ($case in $casingCases) {
        $clashes = @(Get-CasingClash -Ast (Read-ScriptAst -Text $case.Text -Label $case.Name))
        if ($clashes.Count -eq $case.Expect) { Write-Host "  PASS  $($case.Name)" -ForegroundColor Green }
        else {
            Write-Host "  FAIL  $($case.Name) (expected $($case.Expect) clash(es), got $($clashes.Count): $($clashes -join '; '))" -ForegroundColor Red
            $failures++
        }
    }

    try {
        $null = Read-ScriptAst -Text "param([string]`$Foo = ''" -Label 'unparseable'
        Write-Host "  FAIL  a script that does not parse is an error (no error raised)" -ForegroundColor Red
        $failures++
    }
    catch { Write-Host "  PASS  a script that does not parse is an error" -ForegroundColor Green }

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

# Tracked files rather than a directory walk: the scripts are in two places
# (`Scripts/` and `build.ps1` at the root), and a walk would also reach the .ps1
# files that FetchContent and tool downloads leave under build/, which are not ours.
$tracked = @(& git -C $Root ls-files '*.ps1' | Where-Object { $_ } | Sort-Object)
if ($tracked.Count -eq 0) {
    Write-Host "FAIL: git listed no tracked .ps1 files under $Root" -ForegroundColor Red
    exit 1
}

$scripts = $tracked
if ($PSCmdlet.ParameterSetName -eq 'Changed') {
    $change = & (Join-Path $PSScriptRoot 'Get-ChangeTier.ps1') -BaseRef $BaseRef
    $self = 'Scripts/Test-ScriptTraps.ps1'
    # IsFull with no files is Get-ChangeTier's fail-closed answer to a diff it
    # could not compute: check everything rather than nothing.
    if ($change.IsFull -and @($change.ChangedFiles).Count -eq 0) {
        Write-Host "  Diff against $BaseRef unavailable -- checking every script." -ForegroundColor Yellow
    }
    elseif (@($change.ChangedFiles) -contains $self) {
        # A changed detector proves itself first, then holds the whole tree to its rules.
        if (-not (Invoke-SelfTest)) { exit 1 }
        Write-Host "  $self changed -- checking every script." -ForegroundColor DarkGray
    }
    else {
        # Tracked files only: a script the diff deletes has nothing left to check.
        $scripts = @($tracked | Where-Object { @($change.ChangedFiles) -contains $_ })
        if ($scripts.Count -eq 0) {
            Write-Host "==> PowerShell traps: no .ps1 changed since $BaseRef -- nothing to check." -ForegroundColor DarkGray
            exit 0
        }
    }
}

Write-Host "==> PowerShell traps ($($scripts.Count) script(s))" -ForegroundColor Cyan

$violations = 0
foreach ($path in $scripts) {
    $text = Get-Content -LiteralPath (Join-Path $Root $path) -Raw
    try {
        $ast = Read-ScriptAst -Text $text -Label $path
    }
    catch {
        Write-Host "  FAIL  ${path}: $($_.Exception.Message)" -ForegroundColor Red
        $violations++
        continue
    }

    if ((Get-BindingVerdict -Ast $ast) -eq 'unbound') {
        Write-Host "  FAIL  ${path}: param() block without [CmdletBinding()]" -ForegroundColor Red
        $violations++
    }
    foreach ($clash in @(Get-CasingClash -Ast $ast)) {
        Write-Host "  FAIL  ${path}: one variable, two spellings -- $clash" -ForegroundColor Red
        $violations++
    }
}

if ($violations -gt 0) {
    Write-Host ""
    Write-Host "$violations violation(s)." -ForegroundColor Red
    Write-Host "  Binding: add [CmdletBinding()] above param(), or an unrecognised argument is" -ForegroundColor Yellow
    Write-Host "    discarded and the defaults run -- how 'Run-EloMatch.ps1 -?' twice started a match." -ForegroundColor Yellow
    Write-Host "  Spelling: rename one of the variables (write-powershell rule 2); names are" -ForegroundColor Yellow
    Write-Host "    case-insensitive, so the two spellings overwrite or shadow each other." -ForegroundColor Yellow
    exit 1
}

Write-Host "PASS: no PowerShell traps in $($scripts.Count) script(s)." -ForegroundColor Green
exit 0
