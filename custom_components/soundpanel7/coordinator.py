from __future__ import annotations

from datetime import timedelta
import logging

from aiohttp import ClientError

from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .const import DEFAULT_API_PATH, DEFAULT_PORT, DOMAIN

LOGGER = logging.getLogger(__name__)


class SoundPanel7Coordinator(DataUpdateCoordinator[dict]):
    """Coordinate SoundPanel 7 API updates."""

    def __init__(self, hass, host: str, port: int = DEFAULT_PORT, api_path: str = DEFAULT_API_PATH) -> None:
        super().__init__(
            hass,
            logger=LOGGER,
            name=DOMAIN,
            update_interval=timedelta(seconds=5),
        )
        self.host = host
        self.port = port
        self.api_path = api_path or DEFAULT_API_PATH
        self.session = async_get_clientsession(hass)

    @property
    def base_url(self) -> str:
        return f"http://{self.host}:{self.port}"

    @property
    def status_url(self) -> str:
        return f"{self.base_url}{self.api_path}"

    async def _async_update_data(self) -> dict:
        try:
            response = await self.session.get(self.status_url, timeout=10)
            response.raise_for_status()
            payload = await response.json()
        except (ClientError, TimeoutError, ValueError) as err:
            raise UpdateFailed(f"Error fetching {self.status_url}: {err}") from err

        if not isinstance(payload, dict):
            raise UpdateFailed(f"Unexpected payload from {self.status_url}")

        return payload
