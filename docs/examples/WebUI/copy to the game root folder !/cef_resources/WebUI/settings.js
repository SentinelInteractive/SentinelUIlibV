const keys = ["F1","F2","F3","F4","F5","F6","F7","F8","F9","F10","F11","F12","BACKSPACE","TAB","ENTER","ESCAPE","SPACE","PAGEUP","PAGEDOWN","END","HOME","INSERT","DELETE","CAPSLOCK","LEFT","UP","RIGHT","DOWN","SHIFT","CTRL","ALT","LWIN","RWIN","PAUSE","TILDE","PRINT","A","B","C","D","E","F","G","H","I","J","K","L","M","N","O","P","Q","R","S","T","U","V","W","X","Y","Z","0","1","2","3","4","5","6","7","8","9"];

let isExperimentalUnlocked = false;

const settingsData = {
    general: [
        { label: "Url", value: "https://localhost:8080", type: "input", desc: "Target URL or local file path for the WebUI content." },
        { label: "Show On Start", value: "true", options: ["true", "false"], type: "toggle", desc: "Automatically display the UI when the application launches." }
    ],
    display: [
        { label: "UIFrameRateLimit", value: 60, options: [30, 60, 120, 144], type: "toggle", desc: "Limits the UI rendering frame rate (30-144 FPS)." },
        { id: 'enablegpu', label: "Enable GPU", value: "true", options: ["true", "false"], type: "toggle", desc: "Uses the GPU for rendering. Disabling this locks GPU-specific features." },
        { id: 'gpuboost', label: "GPU Boost", value: "true", options: ["true", "false"], type: "toggle", desc: "Enables additional GPU optimizations." },
        { label: "UIPriority", value: "Balanced", options: ["Low", "Balanced", "High"], type: "toggle", desc: "Controls resource consumption aggressiveness." },
        { label: "CacheMode", value: "Balanced", options: ["Off", "Balanced", "Aggressive"], type: "toggle", desc: "Controls data caching to improve loading." },
        { label: "SmoothRendering", value: "true", options: ["true", "false"], type: "toggle", desc: "Improves visual smoothness." }
    ],
    experimental: [
        { label: "RendererProcessLimit", value: 0, options: [0, 1, 2, 3, 4, 5, 6, 7, 8], type: "toggle", desc: "Controls Chromium processes. 0 = Default, Max = 8." },
        { id: 'strictgpu', label: "Strict GPU", value: "false", options: ["true", "false"], type: "toggle", desc: "Prevents fallback to CPU rendering if GPU fails." },
        { label: "EnableDebug", value: "false", options: ["true", "false"], type: "toggle", desc: "Enables logging for troubleshooting. No gameplay impact." },
        { label: "ForceUIScale", value: "1.0", options: ["0.8", "1.0", "1.2", "1.5"], type: "toggle", desc: "Forces UI scale for high DPI or blurry UI fixes." },
        { label: "BackgroundUpdate", value: "true", options: ["true", "false"], type: "toggle", desc: "Keeps UI updating even when not visible." },
        { label: "CompatibilityMode", value: "false", options: ["true", "false"], type: "toggle", desc: "Improves compatibility with older GPUs and drivers." }
    ],
    keybinding: [
        { label: "SettingsToggle", value: "F1", type: "key", desc: "Key used to toggle the WebUI settings menu." },
        { label: "Toggle Visibility", value: "F2", type: "key", desc: "Key used to show or hide the WebUI." },
        { label: "Capture Input", value: "F3", type: "key", desc: "Key used to toggle mouse/keyboard focus." },
        { label: "Refresh", value: "F5", type: "key", desc: "Force reload the current WebUI page." }
    ],
    performance: [],
    about: [
        { label: "Unlock Experimental", value: "false", options: ["true", "false"], type: "toggle", desc: "Unlock the Experimental tab to access dangerous stability settings." },
        { label: "Credits", value: "ShadowOne", type: "static", desc: "Lead developer and project lead." },
        { label: "Community", value: "lelac14, TeamCharlotte, JoyLucien", type: "static", desc: "Members who contributed ideas and support." },
        { label: "Version", value: "0.2.4 Beta", type: "static", desc: "Current build version." },
        { label: "Engine", value: "CEF", type: "static", desc: "Chromium Embedded Framework." },
        { label: "Engine Version", value: "145.0.28+chromium-145.0.7632.160", type: "static", desc: "Detailed build string." }
    ]
};

let activeKeyIndex = null;

function openKeyPicker(index) {
    activeKeyIndex = index;
    const picker = document.getElementById('key-picker');
    const list = document.getElementById('key-list');
    list.innerHTML = '';
    keys.forEach(k => {
        const div = document.createElement('div');
        div.className = 'key-option';
        div.innerText = k;
        div.onclick = () => selectKey(k);
        list.appendChild(div);
    });
    picker.style.display = 'block';
}

function selectKey(key) {
    settingsData.keybinding[activeKeyIndex].value = key;
    closeKeyPicker();
    showSection('keybinding');
}

function closeKeyPicker() { document.getElementById('key-picker').style.display = 'none'; }

function updateValue(section, index) {
    const item = settingsData[section][index];
    if (item.type === "input" || item.type === "static" || isLocked(item)) return;
    if (item.type === "key") { openKeyPicker(index); return; }

    const curIdx = item.options.indexOf(item.value);
    const nextIdx = (curIdx + 1) % item.options.length;
    item.value = item.options[nextIdx];

    if (item.id === 'enablegpu' && item.value === "false") {
        settingsData.display.find(s => s.id === 'gpuboost').value = "false";
        settingsData.experimental.find(s => s.id === 'strictgpu').value = "false";
    }

    if (item.label === "Unlock Experimental") {
        isExperimentalUnlocked = (item.value === "true");
    }

    showSection(section); 
}

function isLocked(item) {
    const gpuEnabled = settingsData.display.find(s => s.id === 'enablegpu').value === "true";
    return !gpuEnabled && (item.id === 'gpuboost' || item.id === 'strictgpu');
}

function showDesc(text) {
    const box = document.getElementById('desc-box');
    const txt = document.getElementById('desc-text');
    if(text) { box.classList.remove('opacity-0'); txt.innerText = text; } 
    else { box.classList.add('opacity-0'); }
}

function saveSettings() {
    let payload = "";
    const getVal = (section, label) => {
        const item = settingsData[section].find(s => s.label === label || s.id === label);
        return item ? item.value : "";
    };

    try {
        payload += `Hotkeys|ToggleVisibility|${getVal('keybinding', 'Toggle Visibility')}*`;
        payload += `Hotkeys|CaptureInput|${getVal('keybinding', 'Capture Input')}*`;
        payload += `Hotkeys|RefreshKey|${getVal('keybinding', 'Refresh')}*`;
        payload += `Hotkeys|SettingsToggle|${getVal('keybinding', 'SettingsToggle')}*`;

        payload += `Browser|Url|${getVal('general', 'Url')}*`;
        payload += `Browser|ShowOnStart|${getVal('general', 'Show On Start')}*`;
        payload += `Browser|EnableGPU|${getVal('display', 'enablegpu')}*`;
        payload += `Browser|GPUBoost|${getVal('display', 'gpuboost')}*`;
        payload += `Browser|UIFrameRateLimit|${getVal('display', 'UIFrameRateLimit')}*`;
        payload += `Browser|StrictGPU|${getVal('experimental', 'strictgpu')}*`;
        payload += `Browser|CompatibilityMode|${getVal('experimental', 'CompatibilityMode')}*`;
        payload += `Browser|RendererProcessLimit|${getVal('experimental', 'RendererProcessLimit')}*`;
        payload += `Browser|ForceUIScale|${getVal('experimental', 'ForceUIScale')}*`;
        payload += `Browser|SmoothRendering|${getVal('display', 'SmoothRendering')}*`;
        payload += `Browser|CacheMode|${getVal('display', 'CacheMode')}*`;
        payload += `Browser|UIPriority|${getVal('display', 'UIPriority')}*`;
        payload += `Browser|BackgroundUpdate|${getVal('experimental', 'BackgroundUpdate')}*`;
        payload += `Browser|EnableDebug|${getVal('experimental', 'EnableDebug')}*`;

        console.log("NUI_SAVE:" + payload);
        
        const btn = document.getElementById('save-btn');
        btn.innerText = "SAVED!";
        btn.classList.add('bg-green-600', 'border-green-600');
        btn.classList.remove('bg-red-700', 'border-red-700');
        setTimeout(() => { 
            btn.innerText = "SAVE CONFIG"; 
            btn.classList.remove('bg-green-600', 'border-green-600'); 
            btn.classList.add('bg-red-700', 'border-red-700'); 
        }, 2000);
    } catch(e) {
        console.error("Payload packing failed: " + e);
    }
}

function showSection(sectionId) {
    const contentBox = document.getElementById('settings-content');
    const warningArea = document.getElementById('warning-area');
    const buttons = document.querySelectorAll('.tab-button');
    buttons.forEach(btn => btn.classList.remove('tab-active'));
    document.getElementById(`btn-${sectionId}`).classList.add('tab-active');

    warningArea.innerHTML = '';
    let html = '';

    if (sectionId === 'experimental') {
        if (!isExperimentalUnlocked) {
            html = '<div class="h-full flex flex-col items-center justify-center text-white/40 p-10 text-center uppercase italic"><p class="mb-4 text-red-500 font-bold">Section Locked</p><p class="text-[0.65rem] tracking-widest leading-relaxed">Please enable "Unlock Experimental" in the About section to access these stability settings.</p></div>';
        } else {
            warningArea.innerHTML = '<div class="warning-banner">⚠️ WARNING: EDITING THESE VALUES IS DANGEROUS AND MAY CAUSE INSTABILITY! ⚠️</div>';
        }
    }

    if (html === '') {
        if (settingsData[sectionId].length === 0) {
            html = '<div class="h-full flex items-center justify-center text-white/10 italic uppercase tracking-[1em]">Empty</div>';
        } else {
            settingsData[sectionId].forEach((item, index) => {
                const isInput = item.type === "input";
                const isStatic = item.type === "static";
                const locked = isLocked(item);

                html += `
                    <div class="setting-row text-white ${locked ? 'disabled' : ''}" onmouseover="showDesc('${item.desc}')" onmouseout="showDesc('')"
                         onclick="updateValue('${sectionId}', ${index})">
                        <span>${item.label}</span>
                        ${isInput ? 
                            `<input type="text" value="${item.value}" spellcheck="false" onchange="settingsData.${sectionId}[${index}].value=this.value">` : 
                            `<span class="val-text ${locked ? 'text-gray-600' : 'text-red-500'} ${isStatic ? 'condensed-text' : ''}">${isStatic ? item.value : '&lt; ' + item.value + ' &gt;'}</span>`
                        }
                    </div>
                `;
            });
        }
    }
    contentBox.innerHTML = html;
}

async function loadIniSettings() {
    try {
        const response = await fetch("../../settings.ini"); 
        if (!response.ok) return;
        const text = await response.text();
        
        const lines = text.split('\n');
        let currentSection = "";
        const ini = {};
        for (let line of lines) {
            line = line.trim();
            if(line.startsWith(";") || line.length === 0) continue;
            if(line.startsWith("[")) { currentSection = line.replace(/[\[\]]/g, ""); continue; }
            
            const parts = line.split("=");
            if(parts.length >= 2) {
                const key = parts[0];
                const val = parts.slice(1).join("=");
                if(!ini[currentSection]) ini[currentSection] = {};
                ini[currentSection][key] = val.replace(/["']/g, '');
            }
        }

        const setVal = (section, id, val) => {
            const item = settingsData[section].find(s => s.label === id || s.id === id);
            if (item) {
                item.value = isNaN(item.value) || typeof item.value === 'string' ? val : parseInt(val);
            }
        };

        if (ini.Browser) {
            setVal('general', 'Url', ini.Browser.Url);
            setVal('general', 'Show On Start', ini.Browser.ShowOnStart);
            setVal('display', 'enablegpu', ini.Browser.EnableGPU);
            setVal('display', 'gpuboost', ini.Browser.GPUBoost);
            setVal('display', 'UIFrameRateLimit', ini.Browser.UIFrameRateLimit);
            setVal('experimental', 'strictgpu', ini.Browser.StrictGPU);
            setVal('experimental', 'CompatibilityMode', ini.Browser.CompatibilityMode);
            setVal('experimental', 'RendererProcessLimit', ini.Browser.RendererProcessLimit);
            setVal('experimental', 'ForceUIScale', ini.Browser.ForceUIScale);
            setVal('display', 'SmoothRendering', ini.Browser.SmoothRendering);
            setVal('display', 'CacheMode', ini.Browser.CacheMode);
            setVal('display', 'UIPriority', ini.Browser.UIPriority);
            setVal('experimental', 'BackgroundUpdate', ini.Browser.BackgroundUpdate);
            setVal('experimental', 'EnableDebug', ini.Browser.EnableDebug);
        }
        if (ini.Hotkeys) {
            setVal('keybinding', 'Toggle Visibility', ini.Hotkeys.ToggleVisibility);
            setVal('keybinding', 'Capture Input', ini.Hotkeys.CaptureInput);
            setVal('keybinding', 'Refresh', ini.Hotkeys.RefreshKey);
            setVal('keybinding', 'SettingsToggle', ini.Hotkeys.SettingsToggle);
        }

        showSection('general'); 
    } catch (err) {
        console.error("Local INI load intercepted by strict policies: ", err);
    }
}

loadIniSettings();
