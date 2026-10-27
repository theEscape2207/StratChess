# Releasing

How the maintainer publishes a versioned engine build for StratChessUI and other UCI clients. The
workflow mechanics are in [CI.md](CI.md#release-releaseyml). What a version number promises is in
[EngineGuide.md](EngineGuide.md#versions-and-requirements).

## Procedure

1. **Bump the version in a PR.** Edit `project(StratChess VERSION X.Y.Z ...)` in `CMakeLists.txt`.
   Before 1.0, a breaking change, an additive feature or a measured strength gain is MINOR, and a
   fix is PATCH. Merge the PR once CI is green.
2. **Tag that commit on `main`:**
   ```
   git fetch origin
   git tag vX.Y.Z origin/main
   git push origin vX.Y.Z
   ```
3. **Dispatch the workflow:** Actions → Release → Run workflow, or
   `gh workflow run release.yml -f tag=vX.Y.Z`. It refuses a tag that is not `vX.Y.Z`, does not
   match `CMakeLists.txt` or is not on `main`'s history. It builds and tests the exe, checks that
   it runs standalone, and stages a **draft** release with `StratChess-X.Y.Z-windows-x64.exe`.
4. **Curate the notes on the draft.** The workflow writes the requirements line, the source commit,
   the compiler and the runner image. Fill in the empty headings:
   - **Integration impact.** Use the PRs labelled `uci-facing` since the previous tag
     (`gh pr list --state merged --label uci-facing`), plus anything breaking under the supported
     UCI contract.
   - **Strength.** Write a line only with a linked measurement that names its reference,
     conditions and interval. Label lab rows as GCC: the lab does not build the shipped clang-cl
     binary.
   - **Fixes.** Take these from the `Docs/Changelog.md` headlines since the previous tag.
5. **Publish** the draft by hand. Release watchers and `/releases.atom` see it from that point.

## Retries and defects

- **A run fails before staging** (a build, test or standalone check): nothing was written. Fix the
  cause, then dispatch again. If the fix needs a new commit, it also needs a new tag, so it ships
  as a new version.
- **A draft exists and needs rebuilding:** delete the draft (not the tag), then dispatch again. The
  workflow only ever creates a release. It refuses a tag that already has one, draft or published,
  so it never changes a release you are reviewing or one you have published.
- **A published release is defective:** never replace its asset. Ship a new PATCH version, and add
  a line to the old release's notes that points to it.
