// Face features for the hub, computed in the browser from MediaPipe Face Landmarker.
// Only these numbers ever leave the page (as integers x1000); the video never does.
// The hub rejects |value| > 2000, so everything is clamped to that range here.

import { FaceLandmarker } from './vendor/vision_bundle.mjs';

const MAX_VALUE = 2000;

// Option 1: 12 of MediaPipe's built-in face scores (0-1 each).
export const BLENDSHAPE_NAMES = [
  'browInnerUp', 'browOuterUpLeft', 'browOuterUpRight', 'browDownLeft', 'browDownRight',
  'mouthSmileLeft', 'mouthSmileRight', 'cheekSquintLeft', 'cheekSquintRight', 'jawOpen',
  'eyeBlinkLeft', 'eyeBlinkRight',
];

// Option 2: 12 distances between face points, divided by the distance between the outer eye
// corners, so they don't depend on how close you sit. Indices are MediaPipe face mesh points.
// "right"/"left" are the person's own sides.
const P = {
  rightEyeOuter: 33, leftEyeOuter: 263,
  rightBrowMid: 105, leftBrowMid: 334, rightBrowInner: 55, leftBrowInner: 285,
  rightLidUp: 159, rightLidDown: 145, leftLidUp: 386, leftLidDown: 374,
  rightMouth: 61, leftMouth: 291, lipUp: 13, lipDown: 14,
  noseTip: 1, chin: 152, forehead: 10,
};
export const LANDMARK_NAMES = [
  'browEyeRight', 'browEyeLeft', 'mouthWidth', 'cornerUpRight', 'cornerUpLeft', 'mouthOpen',
  'eyeOpenRight', 'eyeOpenLeft', 'browGap', 'cornerEyeRight', 'cornerEyeLeft', 'lowerFace',
];

export async function createLandmarker() {
  const base = new URL('vendor/', document.baseURI).href;
  const fileset = {
    wasmLoaderPath: base + 'wasm/vision_wasm_internal.js',
    wasmBinaryPath: base + 'wasm/vision_wasm_internal.wasm',
  };
  const options = (delegate) => ({
    baseOptions: { modelAssetPath: base + 'face_landmarker.task', delegate },
    runningMode: 'VIDEO',
    numFaces: 2,  // so we can pick the largest face
    outputFaceBlendshapes: true,
    outputFacialTransformationMatrixes: false,
  });
  try {
    return await FaceLandmarker.createFromOptions(fileset, options('GPU'));
  } catch (error) {
    console.warn('GPU delegate failed, using CPU:', error);
    return await FaceLandmarker.createFromOptions(fileset, options('CPU'));
  }
}

const clamp = (v) => Math.max(-MAX_VALUE, Math.min(MAX_VALUE, Math.round(v)));

// Index of the largest face in a result, or -1 if there is none.
function largestFace(result) {
  let best = -1;
  let bestWidth = 0;
  (result.faceLandmarks || []).forEach((points, i) => {
    let lo = 1, hi = 0;
    for (const p of points) {
      lo = Math.min(lo, p.x);
      hi = Math.max(hi, p.x);
    }
    if (hi - lo > bestWidth) {
      bestWidth = hi - lo;
      best = i;
    }
  });
  return best;
}

// Returns null if there's no usable face, else
// { blend: int[12], landmark: int[12], points, clamped } (points for drawing only).
export function extractFeatures(result, width, height) {
  const face = largestFace(result);
  if (face < 0) return null;
  const points = result.faceLandmarks[face];
  const shapes = result.faceBlendshapes && result.faceBlendshapes[face];
  if (!shapes || points.length < 468) return null;

  let clamped = 0;
  const scaled = (v) => {
    const c = clamp(v);
    if (c !== Math.round(v)) clamped++;
    return c;
  };

  const scores = {};
  for (const category of shapes.categories) scores[category.categoryName] = category.score;
  const blend = BLENDSHAPE_NAMES.map((name) => scaled((scores[name] || 0) * 1000));

  // Pixel coordinates: landmark x and y are normalized separately to width and height.
  const at = (i) => ({ x: points[i].x * width, y: points[i].y * height });
  const dist = (a, b) => Math.hypot(at(a).x - at(b).x, at(a).y - at(b).y);
  const eye = dist(P.rightEyeOuter, P.leftEyeOuter);
  if (!(eye > 1)) return null;  // face too small or degenerate

  // "Up" on the face, perpendicular to the eye line, so head roll doesn't change corner heights.
  const ax = { x: (at(P.leftEyeOuter).x - at(P.rightEyeOuter).x) / eye, y: (at(P.leftEyeOuter).y - at(P.rightEyeOuter).y) / eye };
  let up = { x: -ax.y, y: ax.x };
  const chinToForehead = { x: at(P.forehead).x - at(P.chin).x, y: at(P.forehead).y - at(P.chin).y };
  if (up.x * chinToForehead.x + up.y * chinToForehead.y < 0) up = { x: -up.x, y: -up.y };
  const lipCenter = { x: (at(P.lipUp).x + at(P.lipDown).x) / 2, y: (at(P.lipUp).y + at(P.lipDown).y) / 2 };
  const cornerUp = (i) => ((at(i).x - lipCenter.x) * up.x + (at(i).y - lipCenter.y) * up.y);

  const raw = [
    dist(P.rightBrowMid, P.rightLidUp),
    dist(P.leftBrowMid, P.leftLidUp),
    dist(P.rightMouth, P.leftMouth),
    cornerUp(P.rightMouth),
    cornerUp(P.leftMouth),
    dist(P.lipUp, P.lipDown),
    dist(P.rightLidUp, P.rightLidDown),
    dist(P.leftLidUp, P.leftLidDown),
    dist(P.rightBrowInner, P.leftBrowInner),
    dist(P.rightMouth, P.rightEyeOuter),
    dist(P.leftMouth, P.leftEyeOuter),
    dist(P.noseTip, P.chin),
  ];
  const landmark = raw.map((value) => scaled((value / eye) * 1000));
  return { blend, landmark, points, clamped };
}

// The points used above, for drawing on the preview.
export const DRAW_POINTS = Object.values(P);
