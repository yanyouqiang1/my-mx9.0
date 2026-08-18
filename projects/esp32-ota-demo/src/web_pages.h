#pragma once

const char INDEX_HTML[] = R"(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 OTA Update</title>
    <style>
        :root {
            --bg-color: #1a1a2e;
            --card-bg: #16213e;
            --primary: #4CAF50;
            --warning: #FF9800;
            --error: #F44336;
            --text: #e0e0e0;
            --text-muted: #888;
        }
        * {
            box-sizing: border-box;
        }
        body {
            font-family: system-ui, -apple-system, sans-serif;
            background: var(--bg-color);
            color: var(--text);
            margin: 0;
            padding: 20px;
            min-height: 100vh;
        }
        .container {
            max-width: 600px;
            margin: 0 auto;
        }
        .header {
            display: flex;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 20px;
            padding: 15px 20px;
            background: var(--card-bg);
            border-radius: 12px;
        }
        .header h1 {
            margin: 0;
            font-size: 1.5rem;
        }
        .status-indicator {
            display: flex;
            align-items: center;
            gap: 8px;
        }
        .status-dot {
            width: 12px;
            height: 12px;
            border-radius: 50%;
            background: var(--primary);
            animation: pulse 2s infinite;
        }
        .status-dot.warning {
            background: var(--warning);
        }
        .status-dot.error {
            background: var(--error);
            animation: none;
        }
        @keyframes pulse {
            0%, 100% { opacity: 1; }
            50% { opacity: 0.5; }
        }
        .card {
            background: var(--card-bg);
            border-radius: 12px;
            padding: 20px;
            margin-bottom: 20px;
        }
        .card h2 {
            margin: 0 0 15px 0;
            font-size: 1.1rem;
            color: var(--text);
        }
        /* Toggle Switch */
        .toggle-row {
            display: flex;
            align-items: center;
            justify-content: space-between;
        }
        .switch {
            position: relative;
            width: 60px;
            height: 34px;
        }
        .switch input {
            opacity: 0;
            width: 0;
            height: 0;
        }
        .slider {
            position: absolute;
            cursor: pointer;
            top: 0;
            left: 0;
            right: 0;
            bottom: 0;
            background-color: #333;
            transition: 0.3s;
            border-radius: 34px;
        }
        .slider:before {
            position: absolute;
            content: "";
            height: 26px;
            width: 26px;
            left: 4px;
            bottom: 4px;
            background-color: white;
            transition: 0.3s;
            border-radius: 50%;
        }
        input:checked + .slider {
            background-color: var(--primary);
        }
        input:checked + .slider:before {
            transform: translateX(26px);
        }
        /* File Upload */
        .upload-area {
            margin-top: 15px;
        }
        .file-input-wrapper {
            position: relative;
            margin-bottom: 15px;
        }
        .file-input {
            display: none;
        }
        .file-label {
            display: block;
            padding: 15px;
            background: #0f3460;
            border: 2px dashed #4CAF50;
            border-radius: 8px;
            text-align: center;
            cursor: pointer;
            transition: background 0.3s;
        }
        .file-label:hover {
            background: #1a4a7a;
        }
        .file-name {
            margin-top: 8px;
            font-size: 0.9rem;
            color: var(--text-muted);
        }
        .btn {
            width: 100%;
            padding: 12px 20px;
            background: var(--primary);
            color: white;
            border: none;
            border-radius: 8px;
            font-size: 1rem;
            cursor: pointer;
            transition: background 0.3s;
        }
        .btn:hover {
            background: #45a049;
        }
        .btn:disabled {
            background: #555;
            cursor: not-allowed;
        }
        .btn.restart {
            background: var(--warning);
            margin-top: 10px;
        }
        .btn.restart:hover {
            background: #e68a00;
        }
        /* Progress Bar */
        .progress-container {
            width: 100%;
            height: 24px;
            background: #333;
            border-radius: 12px;
            overflow: hidden;
            margin-top: 15px;
        }
        .progress-bar {
            height: 100%;
            background: var(--primary);
            width: 0%;
            transition: width 0.3s;
            display: flex;
            align-items: center;
            justify-content: center;
            font-size: 0.85rem;
            color: white;
        }
        .progress-bar.error {
            background: var(--error);
        }
        /* Status Message */
        .status-message {
            margin-top: 15px;
            padding: 10px;
            border-radius: 8px;
            text-align: center;
            display: none;
        }
        .status-message.success {
            display: block;
            background: rgba(76, 175, 80, 0.2);
            color: var(--primary);
        }
        .status-message.error {
            display: block;
            background: rgba(244, 67, 54, 0.2);
            color: var(--error);
        }
        .status-message.info {
            display: block;
            background: rgba(255, 152, 0, 0.2);
            color: var(--warning);
        }
        /* Device Info */
        .info-grid {
            display: grid;
            grid-template-columns: 1fr 1fr;
            gap: 15px;
        }
        .info-item {
            background: #0f3460;
            padding: 12px;
            border-radius: 8px;
        }
        .info-label {
            font-size: 0.8rem;
            color: var(--text-muted);
            margin-bottom: 4px;
        }
        .info-value {
            font-size: 1rem;
            font-weight: 500;
        }
        /* OTA Status */
        .ota-status {
            font-size: 0.95rem;
            color: var(--text-muted);
        }
        .ota-status.enabled {
            color: var(--primary);
        }
        .ota-status.disabled {
            color: var(--error);
        }
    </style>
</head>
<body>
    <div class="container">
        <!-- Header -->
        <div class="header">
            <h1>ESP32 OTA Update</h1>
            <div class="status-indicator">
                <div class="status-dot" id="connectionDot"></div>
                <span id="connectionText">Connected</span>
            </div>
        </div>

        <!-- OTA Control Card -->
        <div class="card">
            <h2>OTA Control</h2>
            <div class="toggle-row">
                <span>Enable OTA Updates</span>
                <label class="switch">
                    <input type="checkbox" id="otaToggle">
                    <span class="slider"></span>
                </label>
            </div>
            <p class="ota-status" id="otaStatus">Loading...</p>
        </div>

        <!-- Firmware Update Card -->
        <div class="card">
            <h2>Firmware Update</h2>
            <div class="upload-area">
                <div class="file-input-wrapper">
                    <input type="file" id="fileInput" class="file-input" accept=".bin">
                    <label for="fileInput" class="file-label">
                        <div>Click to select .bin file</div>
                        <div class="file-name" id="fileName">No file selected</div>
                    </label>
                </div>
                <button class="btn" id="uploadBtn" disabled>Upload Firmware</button>
                <div class="progress-container">
                    <div class="progress-bar" id="progressBar">0%</div>
                </div>
                <div class="status-message" id="uploadStatus"></div>
            </div>
        </div>

        <!-- Device Info Card -->
        <div class="card">
            <h2>Device Information</h2>
            <div class="info-grid">
                <div class="info-item">
                    <div class="info-label">Firmware Version</div>
                    <div class="info-value" id="firmwareVersion">-</div>
                </div>
                <div class="info-item">
                    <div class="info-label">Available Memory</div>
                    <div class="info-value" id="freeMemory">-</div>
                </div>
                <div class="info-item">
                    <div class="info-label">Uptime</div>
                    <div class="info-value" id="uptime">-</div>
                </div>
                <div class="info-item">
                    <div class="info-label">Chip Model</div>
                    <div class="info-value" id="chipModel">-</div>
                </div>
            </div>
        </div>

        <!-- Restart Button -->
        <button class="btn restart" id="restartBtn">Restart Device</button>
    </div>

    <script>
        // DOM Elements
        const otaToggle = document.getElementById('otaToggle');
        const otaStatus = document.getElementById('otaStatus');
        const fileInput = document.getElementById('fileInput');
        const fileName = document.getElementById('fileName');
        const uploadBtn = document.getElementById('uploadBtn');
        const progressBar = document.getElementById('progressBar');
        const uploadStatus = document.getElementById('uploadStatus');
        const restartBtn = document.getElementById('restartBtn');
        const connectionDot = document.getElementById('connectionDot');
        const connectionText = document.getElementById('connectionText');

        // Device info elements
        const firmwareVersion = document.getElementById('firmwareVersion');
        const freeMemory = document.getElementById('freeMemory');
        const uptime = document.getElementById('uptime');
        const chipModel = document.getElementById('chipModel');

        // Helper function to format uptime
        function formatUptime(seconds) {
            const days = Math.floor(seconds / 86400);
            const hours = Math.floor((seconds % 86400) / 3600);
            const minutes = Math.floor((seconds % 3600) / 60);
            const secs = seconds % 60;

            if (days > 0) {
                return `${days}d ${hours}h ${minutes}m`;
            } else if (hours > 0) {
                return `${hours}h ${minutes}m ${secs}s`;
            } else if (minutes > 0) {
                return `${minutes}m ${secs}s`;
            } else {
                return `${secs}s`;
            }
        }

        // Fetch status from server
        async function fetchStatus() {
            try {
                const response = await fetch('/status');
                if (!response.ok) throw new Error('Status request failed');

                const data = await response.json();

                // Update OTA toggle and status
                otaToggle.checked = data.otaEnabled;
                otaStatus.textContent = data.otaEnabled ? 'OTA updates enabled' : 'OTA updates disabled';
                otaStatus.className = 'ota-status ' + (data.otaEnabled ? 'enabled' : 'disabled');

                // Update device info
                firmwareVersion.textContent = data.firmwareVersion || '-';
                freeMemory.textContent = data.freeMemory ? `${data.freeMemory} KB` : '-';
                uptime.textContent = data.uptime ? formatUptime(data.uptime) : '-';
                chipModel.textContent = data.chipModel || '-';

                // Update connection status
                connectionDot.className = 'status-dot';
                connectionText.textContent = 'Connected';

            } catch (error) {
                console.error('Error fetching status:', error);
                connectionDot.className = 'status-dot error';
                connectionText.textContent = 'Disconnected';
            }
        }

        // OTA Toggle handler
        otaToggle.addEventListener('change', async (e) => {
            try {
                const response = await fetch('/ota-control', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({ enabled: e.target.checked })
                });

                if (response.ok) {
                    otaStatus.textContent = e.target.checked ? 'OTA updates enabled' : 'OTA updates disabled';
                    otaStatus.className = 'ota-status ' + (e.target.checked ? 'enabled' : 'disabled');
                    showStatus('OTA setting updated', 'success');
                } else {
                    // Revert toggle on error
                    e.target.checked = !e.target.checked;
                    showStatus('Failed to update OTA setting', 'error');
                }
            } catch (error) {
                e.target.checked = !e.target.checked;
                showStatus('Error: ' + error.message, 'error');
            }
        });

        // File input handler
        fileInput.addEventListener('change', (e) => {
            const file = e.target.files[0];
            if (file) {
                fileName.textContent = file.name;
                uploadBtn.disabled = false;
            } else {
                fileName.textContent = 'No file selected';
                uploadBtn.disabled = true;
            }
        });

        // Show status message
        function showStatus(message, type) {
            uploadStatus.textContent = message;
            uploadStatus.className = 'status-message ' + type;

            // Auto hide after 5 seconds for success/info messages
            if (type !== 'error') {
                setTimeout(() => {
                    uploadStatus.className = 'status-message';
                }, 5000);
            }
        }

        // Upload button handler
        uploadBtn.addEventListener('click', async () => {
            const file = fileInput.files[0];
            if (!file) return;

            // Reset progress bar
            progressBar.style.width = '0%';
            progressBar.textContent = '0%';
            progressBar.classList.remove('error');
            showStatus('', '');
            uploadBtn.disabled = true;
            otaToggle.disabled = true;

            const formData = new FormData();
            formData.append('file', file);

            const xhr = new XMLHttpRequest();

            // Progress handler
            xhr.upload.addEventListener('progress', (e) => {
                if (e.lengthComputable) {
                    const percent = (e.loaded / e.total) * 100;
                    progressBar.style.width = percent + '%';
                    progressBar.textContent = Math.round(percent) + '%';
                }
            });

            // Load handler (success)
            xhr.addEventListener('load', () => {
                if (xhr.status === 200) {
                    showStatus('Upload successful! Restarting...', 'success');
                    progressBar.style.width = '100%';
                    progressBar.textContent = '100%';
                    setTimeout(() => location.reload(), 3000);
                } else if (xhr.status === 400) {
                    showStatus('Error: Invalid firmware file', 'error');
                    progressBar.classList.add('error');
                    uploadBtn.disabled = false;
                    otaToggle.disabled = false;
                } else if (xhr.status === 403) {
                    showStatus('Error: OTA is disabled', 'error');
                    progressBar.classList.add('error');
                    uploadBtn.disabled = false;
                    otaToggle.disabled = false;
                } else {
                    showStatus('Error: Upload failed (status ' + xhr.status + ')', 'error');
                    progressBar.classList.add('error');
                    uploadBtn.disabled = false;
                    otaToggle.disabled = false;
                }
            });

            // Error handler
            xhr.addEventListener('error', () => {
                showStatus('Error: Network error during upload', 'error');
                progressBar.classList.add('error');
                uploadBtn.disabled = false;
                otaToggle.disabled = false;
            });

            // Abort handler
            xhr.addEventListener('abort', () => {
                showStatus('Upload cancelled', 'info');
                uploadBtn.disabled = false;
                otaToggle.disabled = false;
            });

            xhr.open('POST', '/update');
            xhr.send(formData);
        });

        // Restart button handler
        restartBtn.addEventListener('click', async () => {
            if (confirm('Are you sure you want to restart the device?')) {
                try {
                    restartBtn.disabled = true;
                    restartBtn.textContent = 'Restarting...';

                    await fetch('/restart', { method: 'POST' });

                    // Wait a moment then show reloading message
                    setTimeout(() => {
                        showStatus('Device restarting...', 'info');
                    }, 1000);

                    // Try to reconnect after restart
                    setTimeout(() => {
                        location.reload();
                    }, 5000);
                } catch (error) {
                    showStatus('Error: ' + error.message, 'error');
                    restartBtn.disabled = false;
                    restartBtn.textContent = 'Restart Device';
                }
            }
        });

        // Initial status fetch
        fetchStatus();

        // Status polling - every 5 seconds
        setInterval(fetchStatus, 5000);
    </script>
</body>
</html>
)";

const size_t INDEX_HTML_LEN = sizeof(INDEX_HTML);

// Template processor for dynamic values
inline String processor(const String& var) {
    // Reserved for future template processing if needed
    return String();
}
