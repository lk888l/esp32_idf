idf_component_register(
    SRCS
        "src/object_dictionary.cpp"
        "src/sdo_server.cpp"
        "src/pdo.cpp"
        "src/node.cpp"
        "src/standard_profile.cpp"
    INCLUDE_DIRS "include"
    REQUIRES can_transport
)

