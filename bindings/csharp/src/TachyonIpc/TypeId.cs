namespace TachyonIpc;

public static class TypeId
{
    /// <summary>Builds a type_id from route and message type components.</summary>
    /// <param name="routeId">Route identifier, 0–65535. Must be 0 for Bus.</param>
    /// <param name="msgType">Message type, 0–65535.</param>
    public static uint Make(ushort routeId, ushort msgType) =>
        ((uint)routeId << 16) | msgType;

    /// <summary>Extracts the route_id from a type_id </summary>
    public static ushort RouteId(uint typeId) => (ushort)(typeId >> 16);

    /// <summary>Extracts the msg_type from a type_id </summary>
    public static ushort MsgType(uint typeId) => (ushort)(typeId & 0xFFFF);
}