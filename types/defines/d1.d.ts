type D1Result<T = unknown> = {
  results: T[];
  success: true;
  meta: any;
  error?: never;
};

type D1ExecResult = {
  count: number;
  duration: number;
};

declare abstract class D1Database {
  prepare(query: string): D1PreparedStatement;
  dump(): Promise<ArrayBuffer>;
  batch<T = unknown>(statements: D1PreparedStatement[]): Promise<D1Result<T>[]>;
  exec<T = unknown>(query: string): Promise<D1ExecResult>;
}

declare abstract class D1PreparedStatement {
  bind(...values: any[]): D1PreparedStatement;
  first<T = unknown>(colName: string): Promise<T | null>;
  first<T = unknown>(colName: undefined): Promise<Record<string, T> | null>;
  run<T = unknown>(): Promise<D1Result<T>>;
  all<T = unknown>(): Promise<D1Result<T[]>>;
  raw<T = unknown>(): Promise<T[]>;
}
