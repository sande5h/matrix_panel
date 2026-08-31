idf_component_register(SRCS "main.c" "video.c" "gfx.c" "net.c" "tests.c"
                       INCLUDE_DIRS "."
                       REQUIRES hub75 esp_partition esp_wifi esp_netif esp_event nvs_flash)
