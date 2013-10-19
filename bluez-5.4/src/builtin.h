extern struct bluetooth_plugin_desc __bluetooth_builtin_hostname;
extern struct bluetooth_plugin_desc __bluetooth_builtin_wiimote;
extern struct bluetooth_plugin_desc __bluetooth_builtin_audio;
extern struct bluetooth_plugin_desc __bluetooth_builtin_network;
extern struct bluetooth_plugin_desc __bluetooth_builtin_input;
extern struct bluetooth_plugin_desc __bluetooth_builtin_hog;
extern struct bluetooth_plugin_desc __bluetooth_builtin_gatt;
extern struct bluetooth_plugin_desc __bluetooth_builtin_scanparam;
extern struct bluetooth_plugin_desc __bluetooth_builtin_deviceinfo;

static struct bluetooth_plugin_desc *__bluetooth_builtin[] = {
  &__bluetooth_builtin_hostname,
  &__bluetooth_builtin_wiimote,
  &__bluetooth_builtin_audio,
  &__bluetooth_builtin_network,
  &__bluetooth_builtin_input,
  &__bluetooth_builtin_hog,
  &__bluetooth_builtin_gatt,
  &__bluetooth_builtin_scanparam,
  &__bluetooth_builtin_deviceinfo,
  NULL
};
