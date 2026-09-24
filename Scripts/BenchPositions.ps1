<#
.SYNOPSIS
    The fixed-depth benchmark position set and its FEN-file reader, dot-sourced by Run-Bench.ps1
    and Compare-SearchProfile.ps1 so a recorded table and a comparison always read one list.

.NOTES
    No param() block: this is a library. Run-Bench.ps1 -SelfTest covers it; the link is its
    $SelfTestCoverers entry in Validate-PrePR.ps1. Editing the set invalidates every recorded
    table, which is why Run-Bench prints the set's hash.
#>

Set-StrictMode -Version Latest

# Opening, middlegame and endgame, chosen so no position is trivial at depth 12. Every FEN carries
# its side-to-move field, or the engine plays the wrong side.
$DefaultPositions = @(
    @{ Name = 'startpos';    Fen = 'rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1' }
    @{ Name = 'kiwipete';    Fen = 'r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1' }
    @{ Name = 'rook-endgm';  Fen = '2r3k1/1p3pp1/p3p2p/8/2PR4/1P3P2/P4KPP/8 w - - 0 1' }
    @{ Name = 'tactical-4';  Fen = 'r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1' }
    @{ Name = 'tactical-5';  Fen = 'rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8' }
    @{ Name = 'open-mid';    Fen = 'r1bqkb1r/pp3ppp/2n1pn2/2pp4/3P1B2/2PBPN2/PP3PPP/RN1QK2R w KQkq - 0 7' }
    @{ Name = 'closed-mid';  Fen = 'r1bq1rk1/pp2ppbp/2np1np1/8/2PNP3/2N1B3/PP2BPPP/R2QK2R w KQ - 0 9' }
    @{ Name = 'piece-endgm'; Fen = '2r3k1/pp3pp1/4p2p/3n4/3P4/P1NBP3/1P3PPP/2R3K1 w - - 0 1' }
)

# Sparse pawn endgames are deliberately absent. They are nearly solved at these depths, so they
# finish in tens of milliseconds and measure timer resolution rather than the engine.

function Resolve-Positions {
    <#
        The built-in set, or a file of FENs, one per line; blank lines and '#' lines are skipped.
        Wrap the call site in @(): a one-FEN file unrolls to a single hashtable on return.
    #>
    param([string]$Path)

    if (-not $Path) { return $DefaultPositions }

    if (-not (Test-Path $Path)) {
        throw "Positions file not found: $Path"
    }

    $i = 0
    $list = foreach ($line in Get-Content $Path) {
        $fen = $line.Trim()
        if (-not $fen -or $fen.StartsWith('#')) { continue }
        $i++
        @{ Name = "pos-$i"; Fen = $fen }
    }

    if (-not $list) { throw "No FENs found in $Path" }
    return $list
}

function Get-PositionSetHash {
    <# First 12 hex digits of the SHA-256 of the FENs, newline-joined: two tables compare only if it matches. #>
    param([Parameter(Mandatory)][object[]]$List)

    $sha    = [System.Security.Cryptography.SHA256]::Create()
    $joined = ($List | ForEach-Object { $_.Fen }) -join "`n"
    $bytes  = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($joined))
    $sha.Dispose()
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '').Substring(0, 12)
}
