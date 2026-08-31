idf_component_register(SRCS "nowplaying.c" "main.c" "video.c" "gfx.c" "net.c" "tests.c" "usage.c"
                            "screen.c" "server.c"
                       INCLUDE_DIRS "."
                       REQUIRES hub75 esp_partition esp_wifi esp_netif esp_event nvs_flash
                                esp_http_client esp_http_server)
