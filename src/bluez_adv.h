/* bluez_adv.h — register the container's advertisement with host BlueZ, the way Jolla's
 * bridge does (org.bluez LEAdvertisingManager1.RegisterAdvertisement + an exported
 * org.bluez.LEAdvertisement1 object), instead of driving the controller's advertising HCI
 * directly. BlueZ then centrally manages the shared controller's advertising; the resulting
 * connection + GATT still belong to the Android container (routed by the mux).
 */
#ifndef BRIDGE_BLUEZ_ADV_H
#define BRIDGE_BLUEZ_ADV_H

#include <glib.h>
#include <stdint.h>

typedef struct BluezAdv BluezAdv;

/* Connect to the system bus. adapter_path e.g. "/org/bluez/hci1" (the vhci BlueZ manages). */
BluezAdv *bluez_adv_new(const char *adapter_path, GError **error);
void      bluez_adv_free(BluezAdv *ba);

/* (Re)register an advertisement built from raw HCI advertising data (AD structures, as given
 * to LE Set [Extended] Advertising Data) and scan-response data. connectable -> Type=peripheral.
 * Replaces any previous registration. Returns FALSE on error. */
gboolean  bluez_adv_register(BluezAdv *ba, gboolean connectable,
                             const uint8_t *adv, gsize adv_len,
                             const uint8_t *scan_rsp, gsize scan_rsp_len,
                             GError **error);

/* Unregister the current advertisement (if any). */
void      bluez_adv_unregister(BluezAdv *ba);

gboolean  bluez_adv_is_registered(BluezAdv *ba);

#endif
