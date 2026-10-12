#include <libusb-1.0/libusb.h>
#include <stdio.h>
int main(void) {
  libusb_context *ctx; libusb_init(&ctx);
  libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, 0x1df7, 0x2500);
  if (!h) { printf("open failed\n"); return 1; }
  int r = libusb_reset_device(h);
  printf("libusb_reset_device: %d (%s)\n", r, libusb_error_name(r));
  libusb_close(h); libusb_exit(ctx); return 0;
}
