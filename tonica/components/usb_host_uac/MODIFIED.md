Vendored from espressif/usb_host_uac 1.5.0 (Apache-2.0, see LICENSE).

Modifications (tonica, 2026-11-01):
- `stream_tx_xfer_submit`: when the TX ring buffer holds less than one URB, submit the available
  data padded with silence instead of parking the URB, keeping the isochronous stream at exactly the
  nominal rate so adaptive USB DACs do not drift (pitch wobble).
- `uac_host_tx_underflows()`: counter of padded URBs.
- `uac_host_tx_queued_bytes()` (2026-11-05): bytes waiting in the TX ring, so the sink can report its
  real output latency (Sinfónica sync).
- The TX ring buffer is allocated in PSRAM (it is a plain software queue; the URB buffers stay in
  DMA-capable internal RAM).
Each change is marked `tonica modification` (or `tonica:`) in the source.
