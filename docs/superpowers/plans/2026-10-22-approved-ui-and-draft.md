# Approved desktop UI and UNO Draft

Implement the approved design-review lobby and final table-based Draft prototype in the React app. Preserve unrelated working changes. Do not commit or publish. Reuse existing components and local game reducer; add no dependencies.

## Approved behavior

- Bright desktop lobby, original menu background, four visible cards in one centered row, large utility buttons and an on-demand friends drawer at the right.
- Six rank filters with distinct SVG badges; profile and result badges use the same ranks.
- Draft: four initial private cards, four rounds of four shared offers. Picked slots become empty. The fourth player receives the remaining card automatically. Round order ABCD, BCDA, CDAB, DABC. All hands end at eight cards, then ordinary Classic rules resume.
- Table appears before pick overlay; public picks stay sharp, active seat has an arrow/highlight, selected card travels to its owner. Reduced motion uses fades. Human picks time out after 10 seconds.

## Tasks

- [x] Move tested draft state helpers into `client/src/game/draft.js`; add engine tests for exact order, no replenishment, card conservation, stale/out-of-turn rejection and gameplay blocked before completion. Integrate draft state into `engine.js`, `constants.js`, `rules.js` and `useUnoGame.js`.
- [x] Add React `DraftSelection` overlay and public pick trays on `GameTable`; animate cards with cleanup and prevent duplicate input. Pause timers behind settings/when pick UI is closed. Start match clock only once drafting is complete.
- [x] Replace rejected lobby layout in `Lobby.jsx` and `styles/polish.css` with the approved design; add Draft mode and reusable rank SVG assets. Keep current invite/profile/settings/navigation behavior.
- [x] Implement rank-filtered leaderboard and reuse rank badges on TopBar/Profile/MatchResult. Cover rank thresholds with a small test.
- [x] Run unit tests/build, then Chromium smoke checks of actual app: login/lobby/modals, invitations, Classic and team modes, full Draft and final automatic picks, timed picks, animation/reduced motion, settings/restart, result rendering, and common PC sizes. Inspect screenshots for overlapping trays/hand/overlay.

## Review boundaries

The existing app simulates matchmaking and opponents locally. No real server, deployed account system or network multiplayer is introduced. Static previews remain review artifacts, not runtime dependencies. Preserve hidden initial hands and do not count draft time as played match time.

## Verification

- 21 Vitest tests pass, including pure draft rules, engine conservation and rank boundaries.
- Vite production build passes.
- Headless Chromium: actual login/lobby/modals, six rank filters, invitation into Custom, Classic/team tables, all 16 picks, automatic last picks, timed picks, flights/input lock, pause/restart, reduced motion and cleanup pass.
- Complete ranked Draft match reaches result with rank badge and rematch returns to Draft matchmaking.
- Screenshots inspected at 1366x768, 1440x900 and 1920x1080. Public trays stay outside the pick overlay.
- Local simulated opponents only; no live multiplayer validation.

## Layout correction

- Removed the fixed friends rail and centered the full lobby content with the logo.
- Four separate cards are visible; the selected card lifts slightly with a gold border. The fifth mode stays reachable via arrows and mode dots.
- Friends open only from their utility button, in a right-side drawer.
- Chromium assertions pass for four non-overlapping cards, aligned central axes, fifth-mode access, arrow wrap, drawer and invitations at all three desktop sizes.
