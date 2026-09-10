# Camera tracking must preserve world geometry

- Date: 2026-09-09
- Status: fixed in working tree
- Area: Polynomial demo camera
- Severity: Incorrect delivered moving-camera video

## Summary
User correction: move the camera. Render only cameras 2 and 4 again.

## Evidence and Cause
Figmentum DanceMotion::deform subtracted headOffset from actor control points
for followHead, without moving the backdrop. Actor recentering was incorrectly
used to implement camera tracking because the SDK lacked this camera boundary.

## Fix Requirements
Translate eye and target consistently in view matrices and raymarch rays.
Keep actor and backdrop world geometry independent of camera choice.

## Verification
SDK and Figmentum consumer built successfully with MSVC Release. Installed in
body Pictor/build/odorouze-camera-sdk. Moving-only native probes were launched
through Excubitor from the Figmentum body with claim/release. Both recorded
960x960 frames at 0.125px tolerance and zero unresolved-hit diagnostics. Actual
camera eye/target were logged; close and full-body images were inspected.
The consumer no longer applies the camera offset to any actor control point.
Full moving-video capture is in progress. Geometry equality at identical
timestamps across camera settings would have caught this regression.

## Follow-up
Replace moving views in the synchronized quad and deliver the corrected video.
