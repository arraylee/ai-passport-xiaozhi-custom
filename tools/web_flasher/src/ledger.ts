export interface LedgerRow {
  sn: string;
  key: string;
  hw: string;
  release: string;
  timestamp: string;
}

function csv(value: string): string {
  return /[",\r\n]/.test(value) ? `"${value.replaceAll('"', '""')}"` : value;
}

export function downloadLedger(row: LedgerRow): void {
  const header = ["sn", "key", "hw", "release", "timestamp"];
  const values = [row.sn, row.key, row.hw, row.release, row.timestamp];
  const content = `${header.join(",")}\r\n${values.map(csv).join(",")}\r\n`;
  const url = URL.createObjectURL(new Blob([content], { type: "text/csv;charset=utf-8" }));
  const a = document.createElement("a");
  a.href = url;
  a.download = `cardid_${row.sn}.csv`;
  a.click();
  URL.revokeObjectURL(url);
}
