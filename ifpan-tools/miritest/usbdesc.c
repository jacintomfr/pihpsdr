#include <libusb-1.0/libusb.h>
#include <stdio.h>
int main(void) {
  libusb_context *ctx; libusb_init(&ctx);
  printf("libusb %d.%d.%d\n", libusb_get_version()->major, libusb_get_version()->minor, libusb_get_version()->micro);
  libusb_device **list; ssize_t n = libusb_get_device_list(ctx, &list);
  for (ssize_t i = 0; i < n; i++) {
    struct libusb_device_descriptor d; libusb_get_device_descriptor(list[i], &d);
    if (d.idVendor != 0x1df7) continue;
    printf("dev %04x:%04x bcdUSB %x speed %d\n", d.idVendor, d.idProduct, d.bcdUSB, libusb_get_device_speed(list[i]));
    struct libusb_config_descriptor *c;
    if (libusb_get_active_config_descriptor(list[i], &c) == 0) {
      for (int j = 0; j < c->bNumInterfaces; j++)
        for (int a = 0; a < c->interface[j].num_altsetting; a++) {
          const struct libusb_interface_descriptor *id = &c->interface[j].altsetting[a];
          printf(" if %d alt %d class %d eps %d\n", id->bInterfaceNumber, id->bAlternateSetting, id->bInterfaceClass, id->bNumEndpoints);
          for (int e = 0; e < id->bNumEndpoints; e++) {
            const struct libusb_endpoint_descriptor *ep = &id->endpoint[e];
            const char *t[] = {"control","isoc","bulk","interrupt"};
            printf("   ep 0x%02x %s maxpkt %d (mult %d) interval %d\n", ep->bEndpointAddress, t[ep->bmAttributes & 3],
                   ep->wMaxPacketSize & 0x7ff, ((ep->wMaxPacketSize >> 11) & 3) + 1, ep->bInterval);
          }
        }
      libusb_free_config_descriptor(c);
    }
  }
  libusb_free_device_list(list, 1); libusb_exit(ctx); return 0;
}
