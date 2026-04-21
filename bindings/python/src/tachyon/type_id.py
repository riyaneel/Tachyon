def make_type_id(route: int, msg_type: int) -> int:
    """
    Encodes route and msg_type into a single type_id value.

    :param route: Route discriminator, bits [31:16]. Values >= 1 are reserved for RPC.
    :param msg_type: Application-defined message type, bits [15:0].
    :returns: Encoded uint32 type_id.
    :raises ValueError: If either argument is outside [0, 65535].
    """
    if not (0 <= route <= 0xFFFF):
        raise ValueError(f"route must be in [0, 65535], got {route}")
    if not (0 <= msg_type <= 0xFFFF):
        raise ValueError(f"msg_type must be in [0, 65535], got {msg_type}")
    return (route << 16) | msg_type


def route_id(type_id: int) -> int:
    """
    Extracts the route_id from bits [31:16] of type_id.

    :param type_id: Encoded type id value.
    :returns: Route discriminator as an uint16 [0, 65535].
    """
    return (type_id >> 16) & 0xFFFF


def msg_type(type_id: int) -> int:
    """
    Extracts the msg_type from bits [15:0] of type_id.

    :param type_id: Encoded type id value.
    :returns: App message type as an uint16 [0, 65535].
    """
    return type_id & 0xFFFF
