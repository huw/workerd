interface D1Meta {
  duration: number;
  size_after: number;
  rows_read: number;
  rows_written: number;
  last_row_id: number;
  changed_db: boolean;
  changes: number;
}

interface D1Result<T = unknown> {
  results: T[];
  success: true;
  meta: D1Meta & Record<string, unknown>;
  error?: never;
}

interface D1ExecResult {
  count: number;
  duration: number;
}

declare abstract class D1Database {
  prepare(query: string): D1PreparedStatement;
  dump(): Promise<ArrayBuffer>;
  batch<const T extends unknown[]>(statements: D1PreparedStatement[]): Promise<{ [I in keyof T]: D1Result<T[I]>[] }>;
  exec(query: string): Promise<D1ExecResult>;
}

declare abstract class D1PreparedStatement {
  bind(...values: unknown[]): D1PreparedStatement;
  first<T = unknown>(colName: string): Promise<T | null>;
  first<T = Record<string, unknown>>(): Promise<T | null>;
  run<T = Record<string, unknown>>(): Promise<D1Result<T>>;
  all<T = Record<string, unknown>>(): Promise<D1Result<T>>;
  raw<T = unknown[]>(): Promise<T[]>;
}
