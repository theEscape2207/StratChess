# Releasing

How the maintainer publishes a versioned engine build for StratChessUI and other UCI clients. The
workflow mechanics are in [CI.md](CI.md#release-releaseyml). What a version number promises is in
[EngineGuide.md](EngineGuide.md#versions-and-requirements).

## Procedure

1. **Bump the version in a PR.** Edit `project(StratChess VERSION X.Y.Z ...)` in `CMakeLists.txt`,
   choosing MINOR or PATCH by the rules in the
   [supported UCI contract](EngineGuide.md#supported-uci-contract). Merge the PR once CI is green.
2. **Confirm `main` is green after the merge**, then tag that commit:
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
   the compiler and the runner image, and appends the PRs merged since the latest published
   release, with `uci-facing` ones grouped first. Fill in the empty headings:
   - **Highlights.** Two to five bullets an engine consumer acts on or notices. Pick from any
     breaking change (UCI, command line, a dropped OS or CPU), the `uci-facing` group, a measured
     strength change, a fix a client would notice, and a platform or toolchain change: a new
     compiler, CPU requirement or supported OS. Leave internal work to the PR list. Write each in
     one or two sentences for a UI author, naming commands and options but no engine internals,
     PR numbers or measurement links. A strength claim still needs a measurement behind it; lab
     results come from the GCC build, not the shipped exe, so word them as approximate.
   - **Integration impact.** What a client must change, for each breaking change and each
     `uci-facing` PR that needs action, plus any caution or disclaimer. Delete the heading when
     there is nothing to say, as in a first release.
5. **Publish** the draft by hand, but only once the `release.yml` run for the tag has **finished
   successfully** and the draft lists `StratChess-X.Y.Z-windows-x64.exe`. A visible draft alone
   proves nothing: the workflow creates the draft before it uploads the asset, so a draft exists
   while the upload runs and stays behind if it fails. Release watchers and `/releases.atom` see
   the release from the moment it is published.

## Retries and defects

- **A run fails before staging** (a build, test or standalone check): nothing was written. Fix the
  cause, then dispatch again. If the fix needs a new commit, it also needs a new tag, so it ships
  as a new version.
- **The upload failed** (the run failed in `stage` after creating the draft): the draft has no
  asset or an incomplete one. Delete that draft, keep the tag, and dispatch again.
- **A draft exists and needs rebuilding:** delete the draft (not the tag), then dispatch again. The
  workflow refuses a tag that already has a release, draft or published, so it never changes a
  release you are reviewing or one you have published.
- **A published release is defective:** never replace its asset. Ship a new PATCH version, and add
  a line to the old release's notes that points to it.
