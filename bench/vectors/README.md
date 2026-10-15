# Reference vectors

Captured on the bench from the **stock VTun 3.0.2 client** (`bench.sh stock-test`),
session `02BE0C000001`, bench password `c79e01d2e49d80eafd192c043c5142bd5a65b5a9`.

- `stock-client-ether.pcap` — full TCP exchange (handshake + first frames).

## Handshake (each message is exactly 50 bytes, NUL padded)

| Dir | Text |
|---|---|
| S→C | `VTUN server ver 3.X 10/13/2026\n` |
| C→S | `HOST: 02BE0C000001\n` |
| S→C | `OK CHAL: <djhdglocepfoaejahdbnealodcbnjgbl>\n` |
| C→S | `CHAL: <fkeefcihhpgpgakcbgcjhjnkakcnpdhk>\n` |
| S→C | `OK FLAGS: <TeK>\n` |

XOR check: `cl2cs(cs2cl(chal) XOR password)` must equal the client `CHAL`.

## First frames after `OK FLAGS`

- Both sides start with `ECHO_REQ` (`20 00`) and answer with `ECHO_REP` (`40 00`).
- Then data frames, e.g. `00 5a` + 90-byte Ethernet frame (IPv6 MLD from the
  client tap — VTesse drops IPv6, the stock client does not).
