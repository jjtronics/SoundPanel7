const SP7 = {
  async _getJSON(url) {
    const r = await fetch(url, { cache: "no-store" });
    return await r.json();
  },
  async _post(url, data) {
    const body = new URLSearchParams(data);
    const r = await fetch(url, { method: "POST", body });
    return await r.text();
  },

  _zoneColor(db, g, o) {
    if (db < g) return "#22c55e";
    if (db < o) return "#f59e0b";
    return "#ef4444";
  },

  async startDashboard() {
    const tick = async () => {
      const s = await this._getJSON("/api/status");
      const set = await this._getJSON("/api/settings");

      document.getElementById("db").textContent = `${s.db_smooth.toFixed(1)} dB`;
      document.getElementById("leq").textContent = s.leq.toFixed(1);
      document.getElementById("peak").textContent = s.peak.toFixed(1);
      document.getElementById("time").textContent = s.time_hhmm;

      document.getElementById("wifi").textContent = `WiFi: ${s.wifi ? "OK" : "OFF"}`;
      document.getElementById("mqtt").textContent = `MQTT: ${s.mqtt ? "OK" : "OFF"}`;
      document.getElementById("ntp").textContent = `NTP: ${s.ntp ? "OK" : "OFF"}`;

      const min = set.db_min, max = set.db_max;
      const pct = Math.max(0, Math.min(1, (s.db_smooth - min) / (max - min)));
      const bar = document.getElementById("bar");
      bar.style.width = `${(pct * 100).toFixed(1)}%`;
      bar.style.background = this._zoneColor(s.db_smooth, set.th_green, set.th_orange);
    };

    await tick();
    setInterval(tick, 500);
  },

  async startManagement() {
    const s = await this._getJSON("/api/settings");
    const st = await this._getJSON("/api/status");

    document.getElementById("netstate").textContent =
      `WiFi=${st.wifi ? "OK" : "OFF"}  IP=${st.ip}  RSSI=${st.rssi}dBm  Host=${s.hostname}`;

    document.getElementById("hostname").value = s.hostname;
    document.getElementById("ntp").value = s.ntp;

    document.getElementById("mqtt_enabled").value = s.mqtt_enabled ? "1" : "0";
    document.getElementById("mqtt_host").value = s.mqtt_host;
    document.getElementById("mqtt_port").value = String(s.mqtt_port);
    document.getElementById("mqtt_topic").value = s.mqtt_topic;
    document.getElementById("mqtt_interval").value = String(s.mqtt_interval);

    document.getElementById("audio_mock").value = s.audio_mock ? "1" : "0";
    document.getElementById("cal_offset").value = String(s.cal_offset);
    document.getElementById("cal_gain").value = String(s.cal_gain);
    document.getElementById("ema_alpha").value = String(s.ema_alpha);

    document.getElementById("backlight").value = String(s.backlight);
    document.getElementById("th_green").value = String(s.th_green);
    document.getElementById("th_orange").value = String(s.th_orange);
  },

  async saveSettings() {
    const data = {
      hostname: document.getElementById("hostname").value,
      ntp: document.getElementById("ntp").value,

      mqtt_enabled: document.getElementById("mqtt_enabled").value,
      mqtt_host: document.getElementById("mqtt_host").value,
      mqtt_port: document.getElementById("mqtt_port").value,
      mqtt_topic: document.getElementById("mqtt_topic").value,
      mqtt_interval: document.getElementById("mqtt_interval").value,

      audio_mock: document.getElementById("audio_mock").value,
      cal_offset: document.getElementById("cal_offset").value,
      cal_gain: document.getElementById("cal_gain").value,
      ema_alpha: document.getElementById("ema_alpha").value,

      backlight: document.getElementById("backlight").value,
      th_green: document.getElementById("th_green").value,
      th_orange: document.getElementById("th_orange").value,
    };
    await this._post("/api/settings", data);
    alert("Saved");
    location.reload();
  },

  async startWifiPortal() {
    await this._post("/api/wifi_portal", {});
    alert("WiFi portal started. Connect to AP shown on screen/serial.");
  },

  async testPublish() {
    await this._post("/api/mqtt_test", {});
    alert("Test publish triggered");
  },

  async reboot() {
    await this._post("/api/reboot", {});
  },

  async factoryReset() {
    if (!confirm("Factory reset?")) return;
    await this._post("/api/factory", {});
  }
};