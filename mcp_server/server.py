from mcp.server import MCPServer
import psutil

server = MCPServer("industrial Sensors")


@server.tool()
def get_sensor_data(device_id: str) -> dict:
    """Return simulated sensor readings for an industrial device."""
    if (device_id == "pump-01"):
        return {"device_id": "pump-01", "simulated": True, "temperature_c": 87.0,
                "vibration_mm_s": 2.1, "rpm": 1450, "pressure_bar": 4.2}
    else:
        raise ValueError(f"Unknown device: {device_id}")


@server.tool()
def get_system_status() -> dict:
    """Return current CPU and memory usage percentages."""
    cpu_percent = psutil.cpu_percent(interval=1)
    ram_percent = psutil.virtual_memory().percent
    return {"simulated": False, "cpu_percent": cpu_percent, "memory_percent": ram_percent}


if __name__ == "__main__":
    server.run(transport="stdio")
