/** @param {unknown} value @returns {value is Record<string, unknown>} */
export const isRecord = (value) =>
  typeof value === 'object' && value !== null && !Array.isArray(value);
/** @param {unknown} value @returns {Record<string, unknown>} */
export function object(value) {
  if (!isRecord(value)) throw new Error('Invalid response: expected object');
  return value;
}
/** @param {unknown} value */
export function string(value) {
  if (typeof value !== 'string') throw new Error('Invalid response: expected text');
  return value;
}
/** @template T @param {unknown} value @param {(value: unknown) => T} parse @returns {T[]} */
export function array(value, parse) {
  if (!Array.isArray(value)) throw new Error('Invalid response: expected list');
  return value.map(parse);
}
/** @param {unknown} value */
export function boolean(value) {
  if (typeof value !== 'boolean') throw new Error('Invalid response: expected boolean');
  return value;
}
/** @param {unknown} value @param {number} min @param {number} max */
export function number(value, min, max) {
  if (typeof value !== 'number' || !Number.isFinite(value) || value < min || value > max)
    throw new Error('Invalid response: number out of range');
  return value;
}
/** @template {string} T @param {unknown} value @param {readonly T[]} options @returns {T} */
export function choice(value, options) {
  const match = options.find((option) => option === value);
  if (match === undefined) throw new Error('Invalid response: unsupported value');
  return match;
}
