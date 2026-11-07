/** Keep a floating inspector inside the viewport while leaving the node handles free. */
export function clampInfoCard(x: number, y: number, width: number, height: number, viewportWidth: number, viewportHeight: number): { left: number; top: number } {
  const margin = 8;
  return { left: Math.max(margin, Math.min(x, Math.max(margin, viewportWidth - width - margin))),
    top: Math.max(margin, Math.min(y, Math.max(margin, viewportHeight - height - margin))) };
}
