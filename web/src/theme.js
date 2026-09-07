// The palette lives in style.css, in one place, for both themes. CSS elements
// inherit it; a canvas cannot, so it reads it back here. That indirection is
// the point: two hardcoded palettes in two languages drift, and the drift
// shows as a plot that does not match the page around it.
//
// Colours that MEAN something — severity, layer, mode — are not in here. Red
// is over the limit in any light. What is here is everything that is only
// ground and ink, which is exactly what changes when the lights come on.
export function palette(el) {
  const cs = getComputedStyle(el)
  const v = n => cs.getPropertyValue(n).trim()
  // --wash/--glare/--grid are stored as bare "r, g, b" so opacity is per-use
  const at = (n, a) => `rgba(${v(n)}, ${a})`
  return {
    board: v('--board'), edge: v('--board-edge'),
    ink: v('--board-ink'), inkInv: v('--board-ink-inv'),
    copper: v('--copper'), silk: v('--silk'), cool: v('--cool'),
    plotBg: v('--plot-bg'),
    simDm: v('--sim-dm'), simCm: v('--sim-cm'),
    cuFront: v('--cu-front'), cuBack: v('--cu-back'), loop: v('--loop'),
    inner: [v('--cu-in1'), v('--cu-in2'), v('--cu-in3'), v('--cu-in4')],
    heat: { high: v('--heat-high'), medium: v('--heat-med'),
            low: v('--heat-low'), info: v('--tin') },
    high: a => at('--heat-high-rgb', a),
    med:  a => at('--heat-med-rgb', a),
    low:  a => at('--heat-low-rgb', a),
    cu:   a => at('--copper-rgb', a),
    // Which way "brighter" points. The risk map runs from quiet to hot, and
    // hot has to end further from the substrate than it started — white-hot
    // on a dark board, ember-dark on a pale one.
    light: getComputedStyle(document.documentElement)
             .getPropertyValue('color-scheme').trim() === 'light',
    wash: at('--wash', 0.62),
    washLine: a => at('--wash-line', a),
    grid: a => at('--grid', a),
    scrim: a => at('--scrim', a),
    // "glare" is the direction of a highlight, not a colour: light on a dark
    // ground, dark on a light one. Anything meant to catch the eye uses it,
    // and both themes get the same contrast for free.
    glare: a => at('--glare', a),
    mono: v('--mono'),
  }
}

// A canvas holds pixels, not styles: when the theme flips, CSS repaints itself
// and every plot keeps the ground it was drawn on. One observer, shared, tells
// them all to draw again. Returns the disposer to call on unmount.
const redraws = new Set()
let observer = null
export function onThemeChange(fn) {
  redraws.add(fn)
  if (!observer) {
    observer = new MutationObserver(() => { for (const f of redraws) f() })
    observer.observe(document.documentElement, { attributeFilter: ['data-theme'] })
  }
  return () => redraws.delete(fn)
}
