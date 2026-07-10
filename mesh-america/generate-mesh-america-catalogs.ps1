$ErrorActionPreference = 'Stop'

$Repo = 'mikecarper/MeshCore'
$OutputDir = $PSScriptRoot
$Root = Split-Path -Parent $PSScriptRoot

$Catalogs = @(
    @{
        Name = 'non-logging'
        SourceDir = Join-Path $Root '.releases\non-logging'
        Tag = 'v1.16.0-halo-keymind-dev-28ee5d2c'
        Output = Join-Path $OutputDir 'keymind-cascade-v1.16.0-provider.json'
        Description = 'Keymind Cascade MeshCore v1.16.0 dev firmware with Halo/Keymind retry tuning and Cascade defaults. This is the standard build without extra logging.'
    },
    @{
        Name = 'logging'
        SourceDir = Join-Path $Root '.releases\logging'
        Tag = 'logging-v1.16.0-halo-keymind-dev-28ee5d2c'
        Output = Join-Path $OutputDir 'keymind-cascade-logging-v1.16.0-provider.json'
        Description = 'Keymind Cascade MeshCore v1.16.0 dev firmware with Halo/Keymind retry tuning, Cascade defaults, and extra logging enabled for diagnostics.'
    }
)

$RoleDefinitions = [ordered]@{
    companionWifi = [ordered]@{
        name = 'Companion WiFi'
        description = 'Companion radio build that exposes the MeshCore companion interface over Wi-Fi instead of BLE or USB.'
    }
    sensor = [ordered]@{
        name = 'Sensor'
        description = 'Sensor and telemetry build for boards that read and publish sensor data on the mesh.'
    }
    terminalChat = [ordered]@{
        name = 'Terminal Chat'
        description = 'Standalone serial terminal chat build for text messaging from a terminal console.'
    }
}

$RolePatterns = @(
    @{ Suffix = 'logging_repeater_bridge_espnow'; Role = 'repeater'; Title = 'Repeater Bridge ESP-NOW'; SubTitle = 'Logging' },
    @{ Suffix = 'logging_repeater'; Role = 'repeater'; Title = 'Repeater'; SubTitle = 'Logging' },
    @{ Suffix = 'companion_radio_ble_femoff'; Role = 'companionBle'; Title = 'Companion BLE'; SubTitle = 'FEM off' },
    @{ Suffix = 'companion_radio_ble_femon'; Role = 'companionBle'; Title = 'Companion BLE'; SubTitle = 'FEM on' },
    @{ Suffix = 'companion_radio_ble_ps_femoff'; Role = 'companionBle'; Title = 'Companion BLE'; SubTitle = 'Power saving, FEM off' },
    @{ Suffix = 'companion_radio_ble_ps_femon'; Role = 'companionBle'; Title = 'Companion BLE'; SubTitle = 'Power saving, FEM on' },
    @{ Suffix = 'companion_radio_ble_ps'; Role = 'companionBle'; Title = 'Companion BLE'; SubTitle = 'Power saving' },
    @{ Suffix = 'companion_radio_ble_'; Role = 'companionBle'; Title = 'Companion BLE'; SubTitle = $null },
    @{ Suffix = 'companion_radio_ble'; Role = 'companionBle'; Title = 'Companion BLE'; SubTitle = $null },
    @{ Suffix = 'companion_ble'; Role = 'companionBle'; Title = 'Companion BLE'; SubTitle = $null },
    @{ Suffix = 'companion_radio_usb_'; Role = 'companionUsb'; Title = 'Companion USB'; SubTitle = $null },
    @{ Suffix = 'companion_radio_usb'; Role = 'companionUsb'; Title = 'Companion USB'; SubTitle = $null },
    @{ Suffix = 'companion_usb'; Role = 'companionUsb'; Title = 'Companion USB'; SubTitle = $null },
    @{ Suffix = 'comp_radio_usb'; Role = 'companionUsb'; Title = 'Companion USB'; SubTitle = $null },
    @{ Suffix = 'companion_radio_serial'; Role = 'companionUsb'; Title = 'Companion USB'; SubTitle = 'Serial' },
    @{ Suffix = 'companion_radio_wifi'; Role = 'companionWifi'; Title = 'Companion WiFi'; SubTitle = 'Wi-Fi companion interface' },
    @{ Suffix = 'repeater_bridge_rs232_serial1'; Role = 'repeater'; Title = 'Repeater Bridge RS232'; SubTitle = 'Serial 1' },
    @{ Suffix = 'repeater_bridge_rs232_serial2'; Role = 'repeater'; Title = 'Repeater Bridge RS232'; SubTitle = 'Serial 2' },
    @{ Suffix = 'repeater_bridge_rs232'; Role = 'repeater'; Title = 'Repeater Bridge RS232'; SubTitle = $null },
    @{ Suffix = 'repeater_bridge_espnow_'; Role = 'repeater'; Title = 'Repeater Bridge ESP-NOW'; SubTitle = $null },
    @{ Suffix = 'repeater_bridge_espnow'; Role = 'repeater'; Title = 'Repeater Bridge ESP-NOW'; SubTitle = $null },
    @{ Suffix = 'Repeater'; Role = 'repeater'; Title = 'Repeater'; SubTitle = $null },
    @{ Suffix = 'repeater_'; Role = 'repeater'; Title = 'Repeater'; SubTitle = $null },
    @{ Suffix = 'repeatr'; Role = 'repeater'; Title = 'Repeater'; SubTitle = $null },
    @{ Suffix = 'repeater'; Role = 'repeater'; Title = 'Repeater'; SubTitle = $null },
    @{ Suffix = 'room_server_'; Role = 'roomServer'; Title = 'Room Server'; SubTitle = $null },
    @{ Suffix = 'room_server'; Role = 'roomServer'; Title = 'Room Server'; SubTitle = $null },
    @{ Suffix = 'room_svr'; Role = 'roomServer'; Title = 'Room Server'; SubTitle = $null },
    @{ Suffix = 'terminal_chat'; Role = 'terminalChat'; Title = 'Terminal Chat'; SubTitle = 'Serial terminal chat' },
    @{ Suffix = 'sensor'; Role = 'sensor'; Title = 'Sensor'; SubTitle = 'Sensor telemetry node' }
)

$MakerDefinitions = [ordered]@{
    elecrow = [ordered]@{ name = 'Elecrow' }
    ebyte = [ordered]@{ name = 'Ebyte' }
    'gat-iot' = [ordered]@{ name = 'GAT-IoT' }
    generic = [ordered]@{ name = 'Generic' }
    heltec = [ordered]@{ name = 'Heltec' }
    Ikoka = [ordered]@{ name = 'Ikoka' }
    keepteen = [ordered]@{ name = 'Keepteen' }
    lilygo = [ordered]@{ name = 'LilyGo' }
    m5stack = [ordered]@{ name = 'M5Stack' }
    meshadventurer = [ordered]@{ name = 'Meshadventurer' }
    meshimi = [ordered]@{ name = 'Meshimi' }
    meshtiny = [ordered]@{ name = 'Meshtiny' }
    minewsemi = [ordered]@{ name = 'Minewsemi' }
    muziworks = [ordered]@{ name = 'Muzi Works' }
    nibble = [ordered]@{ name = 'Nibble' }
    promicro = [ordered]@{ name = 'ProMicro' }
    rak = [ordered]@{ name = 'RAK Wireless' }
    raspberry = [ordered]@{ name = 'Raspberry Pi' }
    seeed = [ordered]@{ name = 'Seeed Studio' }
    tenstar = [ordered]@{ name = 'Tenstar' }
    tinyrelay = [ordered]@{ name = 'Tiny Relay' }
    uniteng = [ordered]@{ name = 'UnitEng' }
    why2025 = [ordered]@{ name = 'WHY2025' }
}

$DeviceMakerOverrides = @{
    'KeepteenLT1' = 'keepteen'
    'M5Stack_Unit_C6L' = 'm5stack'
    'Mesh_pocket' = 'heltec'
    'Meshimi' = 'meshimi'
    'Meshtiny' = 'meshtiny'
    'Minewsemi_me25ls01' = 'minewsemi'
    'Nano_G2_Ultra' = 'uniteng'
    'nibble_screen_connect' = 'nibble'
    'PicoW' = 'raspberry'
    'ProMicro' = 'promicro'
    'R1Neo' = 'muziworks'
    'SenseCapIndicator-ESPNow' = 'seeed'
    'SenseCap_Solar' = 'seeed'
    'T_Beam_S3_Supreme_SX1262' = 'lilygo'
    't1000e' = 'seeed'
    'Tiny_Relay' = 'tinyrelay'
    'waveshare_rp2040_lora' = 'raspberry'
    'WHY2025_badge' = 'why2025'
    'wio-e5' = 'seeed'
    'wio-e5-mini' = 'seeed'
    'wio_wm1110' = 'seeed'
    'WioTrackerL1' = 'seeed'
    'WioTrackerL1Eink' = 'seeed'
}

$DeviceMakerPatterns = @(
    @{ Prefix = 'Ebyte_'; Maker = 'ebyte' },
    @{ Prefix = 'GAT562_'; Maker = 'gat-iot' },
    @{ Prefix = 'Generic_'; Maker = 'generic' },
    @{ Prefix = 'Heltec_'; Maker = 'heltec' },
    @{ Prefix = 'heltec_'; Maker = 'heltec' },
    @{ Prefix = 'ikoka_'; Maker = 'Ikoka' },
    @{ Prefix = 'LilyGo_'; Maker = 'lilygo' },
    @{ Prefix = 'Meshadventurer_'; Maker = 'meshadventurer' },
    @{ Prefix = 'RAK_'; Maker = 'rak' },
    @{ Prefix = 'Station_'; Maker = 'uniteng' },
    @{ Prefix = 'Tbeam_'; Maker = 'lilygo' },
    @{ Prefix = 'Tenstar_'; Maker = 'tenstar' },
    @{ Prefix = 'ThinkNode_'; Maker = 'elecrow' },
    @{ Prefix = 'Xiao_'; Maker = 'seeed' }
)

$DeviceNameOverrides = @{
    'Ebyte_EoRa-S3' = 'Ebyte EoRa-S3'
    'GAT562_30S_Mesh_Kit' = 'GAT-IoT GAT562 30s'
    'GAT562_Mesh_EVB_Pro' = 'GAT-IoT GAT562 EVB Pro'
    'GAT562_Mesh_Tracker_Pro' = 'GAT-IoT GAT562 Tracker'
    'GAT562_Mesh_Watch13' = 'GAT-IoT GAT562 Watch13'
    'Generic_E22_sx1262' = 'Generic E22'
    'Generic_E22_sx1268' = 'Generic E22'
    'Generic_ESPNOW' = 'Generic ESP-NOW'
    'Heltec_ct62' = 'Heltec CT62'
    'Heltec_E213' = 'Heltec Vision Master E213'
    'Heltec_E290' = 'Heltec Vision Master E290'
    'Heltec_mesh_solar' = 'Heltec MeshSolar / MeshTower'
    'Heltec_t096' = 'Heltec Mesh Node T096'
    'Heltec_t1' = 'Heltec Mesh Node T1'
    'Heltec_t114' = 'Heltec T114'
    'Heltec_t114_without_display' = 'Heltec T114'
    'Heltec_T190' = 'Heltec Vision Master T190'
    'heltec_tracker_v2' = 'Heltec Wireless Tracker v2'
    'heltec_v4' = 'Heltec v4'
    'heltec_v4_3' = 'Heltec v4'
    'heltec_v4_expansionkit' = 'Heltec v4 + Expansion Kit (Touch)'
    'heltec_v4_expansionkit_tft' = 'Heltec v4 + Expansion Kit (Touch)'
    'heltec_v4_tft' = 'Heltec v4'
    'Heltec_Wireless_Paper' = 'Heltec Heltec Wireless Paper'
    'ikoka_nano_nrf_22dbm' = 'Ikoka Nano'
    'ikoka_nano_nrf_30dbm' = 'Ikoka Nano'
    'ikoka_nano_nrf_33dbm' = 'Ikoka Nano'
    'ikoka_handheld_nrf_e22_30dbm' = 'Ikoka Handheld nRF E22 30 dBm'
    'ikoka_handheld_nrf_e22_30dbm_096' = 'Ikoka Handheld nRF E22 30 dBm'
    'ikoka_handheld_nrf_e22_30dbm_096_rotated' = 'Ikoka Handheld nRF E22 30 dBm'
    'ikoka_stick_nrf_22dbm' = 'Ikoka Stick'
    'ikoka_stick_nrf_30dbm' = 'Ikoka Stick'
    'ikoka_stick_nrf_33dbm' = 'Ikoka Stick'
    'KeepteenLT1' = 'Keepteen LT1'
    'LilyGo_T_Impulse_Plus' = 'LilyGo T-Impulse Plus nRF52840'
    'LilyGo_T3S3_sx1262' = 'LilyGo T3 S3 (SX126x)'
    'LilyGo_T3S3_sx1276' = 'LilyGo T3 S3 (SX127x)'
    'LilyGo_TBeam_1W' = 'LilyGo T-Beam 1W'
    'LilyGo_TDeck' = 'LilyGo T-Deck'
    'LilyGo_T-Echo-Lite' = 'LilyGo T-Echo Lite'
    'LilyGo_T-Echo-Lite_non_shell' = 'LilyGo T-Echo Lite'
    'LilyGo_TETH_Elite_sx1262' = 'LilyGo T-ETH Elite (SX1262)'
    'LilyGo_TLora_V2_1_1_6' = 'LilyGo LoRa32 V2.1_1.6'
    'LilyGo_Tlora_C6' = 'LilyGo T-LoRa C6'
    'M5Stack_Unit_C6L' = 'M5Stack Unit C6L'
    'Meshadventurer_sx1262' = 'Meshadventurer'
    'Meshadventurer_sx1268' = 'Meshadventurer'
    'Meshimi' = 'Meshimi'
    'Meshtiny' = 'Meshtiny'
    'Mesh_pocket' = 'Heltec MeshPocket'
    'Minewsemi_me25ls01' = 'Minewsemi ME25LS01'
    'Nano_G2_Ultra' = 'UnitEng Nano G2 Ultra'
    'nibble_screen_connect' = 'Nibble Screen Connect'
    'PicoW' = 'Raspberry Pi Pico W'
    'ProMicro' = 'ProMicro nrf52 (faketec)'
    'R1Neo' = 'Muzi Works R1 Neo'
    'RAK_11310' = 'RAK 11310'
    'RAK_3112' = 'RAK WisBlock 3112'
    'RAK_3401' = 'RAK WisMesh 1W Booster (3401 + 13302)'
    'RAK_3x72' = 'RAK 3x72'
    'RAK_4631' = 'RAK WisBlock / WisMesh (RAK 4631)'
    'SenseCapIndicator-ESPNow' = 'Seeed Studio SenseCAP Indicator ESP-NOW'
    'SenseCap_Solar' = 'Seeed Studio SenseCAP Solar'
    'Station_G2' = 'UnitEng Station G2'
    'Station_G3_ESP32' = 'UnitEng/BQ Voyage Station G3'
    'T_Beam_S3_Supreme_SX1262' = 'LilyGo T-Beam Supreme (SX1262)'
    't1000e' = 'Seeed Studio SenseCAP T1000-E'
    'Tbeam_SX1262' = 'LilyGo T-Beam (SX1262)'
    'Tbeam_SX1276' = 'LilyGo T-Beam 1.2 (SX1276)'
    'Tenstar_C3_sx1262' = 'Tenstar C3'
    'Tenstar_C3_sx1268' = 'Tenstar C3'
    'ThinkNode_M1' = 'Elecrow ThinkNode M1'
    'ThinkNode_M2' = 'Elecrow ThinkNode M2'
    'ThinkNode_M3' = 'Elecrow ThinkNode M3'
    'ThinkNode_M5' = 'Elecrow ThinkNode M5'
    'ThinkNode_M6' = 'Elecrow ThinkNode M6'
    'Tiny_Relay' = 'Tiny Relay'
    'waveshare_rp2040_lora' = 'RPI Pico 2040 + WaveShare SX1262'
    'WHY2025_badge' = 'WHY2025 Badge'
    'wio_wm1110' = 'Seeed Studio Wio WM1110'
    'wio-e5' = 'Seeed Studio Wio-E5'
    'wio-e5-mini' = 'Seeed Studio Wio-E5'
    'WioTrackerL1' = 'Seeed Studio Wio Tracker L1 Pro'
    'WioTrackerL1Eink' = 'Seeed Studio Wio Tracker L1 EINK'
    'Xiao_C3' = 'Seeed Studio Xiao C3'
    'Xiao_C6' = 'Seeed Studio Xiao C6'
    'Xiao_nrf52' = 'Seeed Studio Xiao nRF52 WIO'
    'Xiao_rp2040' = 'Seeed Studio Xiao RP2040'
    'Xiao_S3' = 'Seeed Studio Xiao S3'
    'Xiao_S3_WIO' = 'Seeed Studio Xiao S3 WIO'
}

$DeviceSubTitleOverrides = @{
    'Generic_E22_sx1262' = 'SX1262'
    'Generic_E22_sx1268' = 'SX1268'
    'Heltec_t114_without_display' = 'Without display'
    'heltec_v4_3' = 'v4.3'
    'heltec_v4_tft' = 'TFT'
    'heltec_v4_expansionkit' = 'Expansion Kit'
    'heltec_v4_expansionkit_tft' = 'Expansion Kit TFT'
    'LilyGo_TBeam_1W' = '1W PA pins: DIO1 1, NSS 15, BUSY 38'
    'LilyGo_T-Echo-Lite_non_shell' = 'Non shell'
    'ikoka_nano_nrf_22dbm' = '22dBm'
    'ikoka_nano_nrf_30dbm' = '30dBm'
    'ikoka_nano_nrf_33dbm' = '33dBm'
    'ikoka_handheld_nrf_e22_30dbm' = 'No display'
    'ikoka_handheld_nrf_e22_30dbm_096' = '0.96 display'
    'ikoka_handheld_nrf_e22_30dbm_096_rotated' = '0.96 display rotated'
    'ikoka_stick_nrf_22dbm' = '22dBm'
    'ikoka_stick_nrf_30dbm' = '30dBm'
    'ikoka_stick_nrf_33dbm' = '33dBm'
    'Meshadventurer_sx1262' = 'SX1262'
    'Meshadventurer_sx1268' = 'SX1268'
    'Tenstar_C3_sx1262' = 'SX1262'
    'Tenstar_C3_sx1268' = 'SX1268'
    'wio-e5' = 'Wio-E5'
    'wio-e5-mini' = 'Wio-E5 mini'
    'Xiao_S3' = 'SX1262 pins: DIO1 2, NSS 5, BUSY 4'
}

$DeviceVariantInTitle = @{
    'Generic_E22_sx1262' = $true
    'Generic_E22_sx1268' = $true
    'ikoka_nano_nrf_22dbm' = $true
    'ikoka_nano_nrf_30dbm' = $true
    'ikoka_nano_nrf_33dbm' = $true
    'ikoka_handheld_nrf_e22_30dbm' = $true
    'ikoka_handheld_nrf_e22_30dbm_096' = $true
    'ikoka_handheld_nrf_e22_30dbm_096_rotated' = $true
    'ikoka_stick_nrf_22dbm' = $true
    'ikoka_stick_nrf_30dbm' = $true
    'ikoka_stick_nrf_33dbm' = $true
    'Meshadventurer_sx1262' = $true
    'Meshadventurer_sx1268' = $true
    'Tenstar_C3_sx1262' = $true
    'Tenstar_C3_sx1268' = $true
    'wio-e5' = $true
    'wio-e5-mini' = $true
}

$DeviceSelectionNotes = @{
    'LilyGo_TBeam_1W' = 'Board selection note: Use this only for the LilyGo T-Beam 1W high-power PA board. It is not the standard T-Beam SX1262 target: this build uses LoRa DIO1=1, NSS=15, RESET=3, BUSY=38, SCLK/MISO/MOSI=13/12/11, RXEN=21, and GPS TX/RX=5/6. The standard T-Beam SX1262 target uses DIO1=33, NSS=18, RESET=23, SCLK/MISO/MOSI=5/19/27, and GPS RX/TX=12/34.'
    'Xiao_S3' = 'Board selection note: Use this for a Seeed XIAO ESP32S3 with an external SX1262 wired to DIO1=2, NSS=5, RESET=3, BUSY=4, SCLK/MISO/MOSI=7/8/9, and RXEN=6. Do not use it for the XIAO S3 WIO board, which uses DIO1=39, NSS=41, RESET=42, BUSY=40, and RXEN=38.'
}

$RoleSelectionNotes = @{
    companionWifi = 'Role note: Companion WiFi exposes the MeshCore companion interface over Wi-Fi rather than BLE or USB. Use it for boards that should be controlled over a local Wi-Fi network. It is flashable, but the validator treats it as a custom role, so the standard Configure step may not appear.'
    sensor = 'Role note: Sensor builds publish board-supported sensor or telemetry data on the mesh. They are flashable custom-role builds, not standard companion or repeater firmware.'
    terminalChat = 'Role note: Terminal Chat is a standalone serial-terminal text chat build for boards used from a terminal console. It is flashable, but it does not map to the standard Configure step.'
}

function ConvertTo-DeviceName {
    param([string]$Prefix)
    if ($DeviceNameOverrides.ContainsKey($Prefix)) {
        return $DeviceNameOverrides[$Prefix]
    }

    $name = $Prefix.Trim('_')
    $name = $name -replace '_', ' '
    $name = $name -replace '\s+', ' '
    return $name.Trim()
}

function Get-DeviceMakerKey {
    param([string]$DeviceKey)

    if ($DeviceMakerOverrides.ContainsKey($DeviceKey)) {
        return $DeviceMakerOverrides[$DeviceKey]
    }

    foreach ($pattern in $DeviceMakerPatterns) {
        if ($DeviceKey.StartsWith($pattern.Prefix, [StringComparison]::OrdinalIgnoreCase)) {
            return $pattern.Maker
        }
    }

    return 'generic'
}

function Join-SubTitle {
    param(
        [string]$DeviceKey,
        [string]$RoleSubTitle
    )

    $parts = @()
    if ($DeviceSubTitleOverrides.ContainsKey($DeviceKey) -and -not $DeviceVariantInTitle.ContainsKey($DeviceKey)) {
        $parts += $DeviceSubTitleOverrides[$DeviceKey]
    }
    if ($RoleSubTitle) {
        $parts += $RoleSubTitle
    }

    if (-not $parts.Count) {
        return $null
    }

    return ($parts -join ', ')
}

function Join-FirmwareTitle {
    param(
        [string]$DeviceKey,
        [string]$Role,
        [string]$Title
    )

    if (-not $DeviceVariantInTitle.ContainsKey($DeviceKey)) {
        return $Title
    }

    $variant = $DeviceSubTitleOverrides[$DeviceKey]
    if (-not $variant) {
        return $Title
    }

    if ($Role -in @('companionBle', 'companionUsb')) {
        return "Companion radio - $variant"
    }

    return "$Title - $variant"
}

function Get-VariantSortRank {
    param([string]$DeviceKey)

    if (-not $DeviceVariantInTitle.ContainsKey($DeviceKey) -or -not $DeviceSubTitleOverrides.ContainsKey($DeviceKey)) {
        return 1000
    }

    $variant = $DeviceSubTitleOverrides[$DeviceKey]
    if ($variant -match '^(\d+)dBm$') {
        return [int]$Matches[1]
    }

    return 1000
}

function Get-FirmwareGroupVariantSortRank {
    param([array]$Entries)

    $ranks = @($Entries | ForEach-Object { Get-VariantSortRank $_.DeviceKey } | Sort-Object)
    if ($ranks.Count) {
        return $ranks[0]
    }

    return 1000
}

function Join-VersionNotes {
    param(
        [string]$Description,
        [string[]]$DeviceKeys,
        [string]$Role
    )

    $parts = @($Description)
    foreach ($deviceKey in @($DeviceKeys | Sort-Object -Unique)) {
        if ($DeviceSelectionNotes.ContainsKey($deviceKey)) {
            $parts += $DeviceSelectionNotes[$deviceKey]
        }
    }
    if ($RoleSelectionNotes.ContainsKey($Role)) {
        $parts += $RoleSelectionNotes[$Role]
    }

    return ($parts -join "`n`n")
}

function Get-RoleInfo {
    param([string]$DeviceRole)

    foreach ($pattern in $RolePatterns) {
        $suffix = $pattern.Suffix
        if ($DeviceRole.EndsWith("_$suffix", [StringComparison]::OrdinalIgnoreCase) -or
            $DeviceRole.EndsWith($suffix, [StringComparison]::OrdinalIgnoreCase)) {
            $prefixLength = $DeviceRole.Length - $suffix.Length
            if ($prefixLength -gt 0 -and ($DeviceRole[$prefixLength - 1] -eq '_' -or $DeviceRole[$prefixLength - 1] -eq '-')) {
                $prefixLength--
            }

            $devicePrefix = $DeviceRole.Substring(0, $prefixLength).Trim('_')
            if (-not $devicePrefix) {
                throw "Unable to parse device name from '$DeviceRole'"
            }

            return [ordered]@{
                DeviceKey = $devicePrefix
                DeviceName = ConvertTo-DeviceName $devicePrefix
                Role = $pattern.Role
                Title = $pattern.Title
                SubTitle = $pattern.SubTitle
            }
        }
    }

    throw "Unable to parse role from '$DeviceRole'"
}

function Get-FileSortRank {
    param([string]$Type, [string]$Name)

    switch ($Type) {
        'flash-wipe' { return 10 }
        'flash-update' { return 20 }
        'flash' { return 30 }
        default {
            if ($Name.EndsWith('.uf2', [StringComparison]::OrdinalIgnoreCase)) { return 40 }
            if ($Name.EndsWith('.hex', [StringComparison]::OrdinalIgnoreCase)) { return 50 }
            return 60
        }
    }
}

function Get-DeviceType {
    param([array]$Files)

    $extensions = @($Files | ForEach-Object { $_.Extension.ToLowerInvariant() } | Sort-Object -Unique)
    $hasMergedBin = @($Files | Where-Object {
        $_.Extension -ieq '.bin' -and $_.BaseName.EndsWith('-merged', [StringComparison]::OrdinalIgnoreCase)
    }).Count -gt 0

    if ($hasMergedBin) { return 'esp32' }
    if ($extensions -contains '.zip') { return 'nrf52' }
    return 'noflash'
}

function Get-FileType {
    param(
        [System.IO.FileInfo]$File,
        [string]$DeviceType
    )

    $extension = $File.Extension.ToLowerInvariant()
    $isMerged = $File.BaseName.EndsWith('-merged', [StringComparison]::OrdinalIgnoreCase)

    if ($DeviceType -eq 'esp32' -and $extension -eq '.bin' -and $isMerged) { return 'flash-wipe' }
    if ($DeviceType -eq 'esp32' -and $extension -eq '.bin') { return 'flash-update' }
    if ($DeviceType -eq 'nrf52' -and $extension -eq '.zip') { return 'flash' }
    return 'download'
}

function Get-FileTitle {
    param([string]$Type, [string]$Name)

    switch ($Type) {
        'flash-wipe' { return 'Full install (bootloader + firmware)' }
        'flash-update' { return 'Update (app only)' }
        'flash' { return 'Serial DFU package' }
        default {
            if ($Name.EndsWith('.uf2', [StringComparison]::OrdinalIgnoreCase)) { return 'UF2 download' }
            if ($Name.EndsWith('.hex', [StringComparison]::OrdinalIgnoreCase)) { return 'HEX download' }
            return 'Download'
        }
    }
}

function Get-DownloadUrl {
    param(
        [string]$Tag,
        [string]$Name
    )

    $escapedName = [Uri]::EscapeDataString($Name).Replace('%2F', '/')
    return "https://github.com/$Repo/releases/download/$Tag/$escapedName"
}

function New-Catalog {
    param([hashtable]$Definition)

    if (-not (Test-Path -LiteralPath $Definition.SourceDir)) {
        throw "Source directory not found: $($Definition.SourceDir)"
    }

    $sourceFiles = @(Get-ChildItem -LiteralPath $Definition.SourceDir -File | Sort-Object Name)
    if (-not $sourceFiles.Count) {
        throw "No release files found in $($Definition.SourceDir)"
    }

    $parsedFiles = foreach ($file in $sourceFiles) {
        $stem = $file.BaseName
        if ($stem.EndsWith('-merged', [StringComparison]::OrdinalIgnoreCase)) {
            $stem = $stem.Substring(0, $stem.Length - '-merged'.Length)
        }

        $suffix = "-$($Definition.Tag)"
        if (-not $stem.EndsWith($suffix, [StringComparison]::Ordinal)) {
            throw "File '$($file.Name)' does not end with expected tag '$($Definition.Tag)'"
        }

        $deviceRole = $stem.Substring(0, $stem.Length - $suffix.Length)
        $roleInfo = Get-RoleInfo $deviceRole

        [pscustomobject]@{
            File = $file
            DeviceKey = $roleInfo.DeviceKey
            MakerKey = Get-DeviceMakerKey $roleInfo.DeviceKey
            DeviceName = $roleInfo.DeviceName
            Role = $roleInfo.Role
            Title = Join-FirmwareTitle $roleInfo.DeviceKey $roleInfo.Role $roleInfo.Title
            SubTitle = Join-SubTitle $roleInfo.DeviceKey $roleInfo.SubTitle
        }
    }

    $devices = New-Object System.Collections.ArrayList
    foreach ($deviceGroup in @($parsedFiles | Group-Object DeviceName | Sort-Object Name)) {
        $deviceFiles = @($deviceGroup.Group | ForEach-Object { $_.File })
        $deviceMakerKeys = @($deviceGroup.Group | ForEach-Object { $_.MakerKey } | Sort-Object -Unique)
        if ($deviceMakerKeys.Count -ne 1) {
            throw "Device '$($deviceGroup.Name)' maps to multiple makers: $($deviceMakerKeys -join ', ')"
        }

        $deviceType = Get-DeviceType $deviceFiles
        $firmware = New-Object System.Collections.ArrayList

        $firmwareGroups = @(
            $deviceGroup.Group |
                Group-Object Role,Title,SubTitle |
                Sort-Object `
                    @{ Expression = { Get-FirmwareGroupVariantSortRank $_.Group } },
                    @{ Expression = { $_.Name } }
        )

        foreach ($roleGroup in $firmwareGroups) {
            $first = $roleGroup.Group[0]
            $files = @(
                $roleGroup.Group |
                    Sort-Object @{ Expression = { Get-FileSortRank (Get-FileType $_.File $deviceType) $_.File.Name } }, @{ Expression = { $_.File.Name } } |
                    ForEach-Object {
                        $type = Get-FileType $_.File $deviceType
                        [ordered]@{
                            type = $type
                            name = $_.File.Name
                            url = Get-DownloadUrl $Definition.Tag $_.File.Name
                            title = Get-FileTitle $type $_.File.Name
                        }
                    }
            )

            $firmwareEntry = [ordered]@{
                role = $first.Role
                title = $first.Title
            }

            if ($first.SubTitle) {
            $firmwareEntry.subTitle = $first.SubTitle
            }

            $versionNotes = Join-VersionNotes -Description $Definition.Description -DeviceKeys @($roleGroup.Group | ForEach-Object { $_.DeviceKey }) -Role $first.Role
            $firmwareEntry.version = [ordered]@{
                $Definition.Tag = [ordered]@{
                    notes = $versionNotes
                    files = $files
                }
            }

            [void]$firmware.Add($firmwareEntry)
        }

        [void]$devices.Add([ordered]@{
            maker = $deviceMakerKeys[0]
            name = $deviceGroup.Group[0].DeviceName
            type = $deviceType
            firmware = @($firmware)
        })
    }

    $makerMap = [ordered]@{}
    foreach ($makerKey in @($devices | ForEach-Object { $_['maker'] } | Sort-Object -Unique)) {
        if (-not $MakerDefinitions.Contains($makerKey)) {
            throw "No maker definition for '$makerKey'"
        }
        $makerMap[$makerKey] = $MakerDefinitions[$makerKey]
    }

    return [ordered]@{
        description = $Definition.Description
        maker = $makerMap
        role = $RoleDefinitions
        device = @($devices)
    }
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

foreach ($catalogDef in $Catalogs) {
    $catalog = New-Catalog $catalogDef
    $json = ($catalog | ConvertTo-Json -Depth 100) -replace "`r`n", "`n"
    [System.IO.File]::WriteAllText($catalogDef.Output, $json + "`n", [System.Text.UTF8Encoding]::new($false))
    Write-Output ("Wrote {0}" -f $catalogDef.Output)
}
