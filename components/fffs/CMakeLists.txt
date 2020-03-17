idf_component_register(SRCS "src/fffs.c"
                            "src/fffs_utils.c"
                            "src/fffs_disk.c"
                            "src/fffs_rtos.c"

                    INCLUDE_DIRS "include"
                                 "."
                    REQUIRES sdmmc)