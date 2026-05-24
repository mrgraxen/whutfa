import { timingSafeEqual } from "node:crypto";

export function validateToken(required: string | undefined, provided: string | undefined): boolean {
  if (!required) return true;
  if (!provided) return false;
  const a = Buffer.from(required);
  const b = Buffer.from(provided);
  if (a.length !== b.length) return false;
  return timingSafeEqual(a, b);
}

export function tokenFromUrl(url: string): string | undefined {
  try {
    const u = new URL(url, "http://localhost");
    return u.searchParams.get("token") ?? undefined;
  } catch {
    return undefined;
  }
}
