#include <fbs/attributes.h>
#include <stdio.h>
int main(void) {
    fbs_attr_config config = fbs_attr_config_default();
    fbs_attr *context = NULL;
    if (fbs_attr_create(&config, NULL, &context) != 0) return 1;
    printf("API version: %u\n", fbs_attr_version());
    fbs_attr_destroy(context);
    return 0;
}
