// app.js

import { exec, toast } from 'kernelsu';
import { parseLogContent, updateLocalStorage, getStoredData, clearStoredData } from './logParser.js';
import { Ripple, initMDB } from 'mdb-ui-kit/js/mdb.es.min.js';
import Chart from 'chart.js/auto';

// 初始化 MDB UI Kit 的 Ripple 效果
window.Ripple = Ripple;
initMDB({ Ripple });

document.addEventListener('DOMContentLoaded', async () => {
    // --- DOM 元素获取 ---
    const configForm = document.getElementById('config-form');
    const retentionDaysInput = document.getElementById('retention-days');
    const cleanIntervalInput = document.getElementById('clean-interval');
    const editBlacklist1Btn = document.getElementById('edit-blacklist1');
    const editBlacklist2Btn = document.getElementById('edit-blacklist2');
    const editWhitelistBtn = document.getElementById('edit-whitelist');
    const refreshLogBtn = document.getElementById('refresh-log');
    const deleteLogBtn = document.getElementById('delete-log');
    const clearDataBtn = document.getElementById('clear-data');
    const restartModuleBtn = document.getElementById('restart-module');
    const gcStatusSpan = document.getElementById('gc-status');
    const dateSelect = document.getElementById('date-select');
    const appStatsList = document.getElementById('app-stats-list');
    const appStatsTitle = document.getElementById('app-stats-title');
    const f2fsGcInfoContainer = document.getElementById('f2fs-gc-info-container');
    const gcControlButton = document.getElementById('gc-control-btn');
    const f2fsGcConfigContainer = document.getElementById('f2fs-gc-config-container');
    const f2fsGcConfigToggle = document.getElementById('f2fs-gc-config-toggle');
    const f2fsGcConfigToggleLabel = document.getElementById('f2fs-gc-config-toggle-label');
    const cleanNowBtn = document.getElementById('clean-now-btn');

    // --- 状态变量 ---
    let isExt4 = false;
    let gcInfoIntervalId = null;
    let lastChartData = { dirty: -1, free: -1 };

    // --- 辅助函数 ---
    const delay = ms => new Promise(res => setTimeout(res, ms));

    /**
     * 通过执行 udp_client 程序发送命令
     * @param {string} command 要发送的命令名称
     * @returns {Promise<boolean>} 命令是否成功执行
     */
    async function sendUdpCommand(command) {
        try {
            const clientPath = '/data/adb/modules/Clean-C/udp_client';
            const { errno, stdout, stderr } = await exec(`${clientPath} ${command}`);

            if (errno === 0) {
                toast(`命令 '${command}' 已发送。`);
                return true;
            } else {
                toast(`发送命令失败: ${stderr || '未知错误'}`);
                console.error(`UDP 命令发送失败: ${stderr}`);
                return false;
            }
        } catch (error) {
            toast(`执行命令发送程序失败: ${error.message}`);
            console.error(`执行 udp_client 失败: ${error.message}`);
            return false;
        }
    }

    // --- F2FS 信息获取与更新 ---
    async function getRealtimeF2fsInfo() {
        try {
            const command = `
                DATA_DEVICE=$(getprop dev.mnt.dev.data)
                if [ -z "$DATA_DEVICE" ]; then exit 1; fi
                FS_TYPE=$(mount | grep " /data " | awk '{print $5}')
                SYSFS_PATH="/sys/fs/f2fs/$DATA_DEVICE"
                if [ "$FS_TYPE" = "mifs" ]; then SYSFS_PATH="/sys/fs/mifs/$DATA_DEVICE"; fi
                if [ ! -d "$SYSFS_PATH" ]; then exit 1; fi
                cat "$SYSFS_PATH/dirty_segments"
                echo "---SPLIT---"
                cat "$SYSFS_PATH/free_segments"
                echo "---SPLIT---"
                cat "$SYSFS_PATH/gc_urgent"
            `;
            const { errno, stdout } = await exec(command);
            if (errno === 0) {
                const parts = stdout.split('---SPLIT---');
                return {
                    dirty_segments: parts[0]?.trim(),
                    free_segments: parts[1]?.trim(),
                    gc_urgent: parts[2]?.trim(),
                };
            }
        } catch (e) {
            console.warn("获取 F2FS 信息失败，可能不是 F2FS 文件系统或路径不存在:", e);
        }
        return null;
    }

    async function checkFileSystem() {
        let fsType = null;
        try {
            const { errno, stdout } = await exec(`mount | grep " /data " | awk '{print $5}'`);
            if (errno === 0) {
                fsType = stdout.trim();
                isExt4 = fsType === 'ext4';
            } else {
                isExt4 = true;
            }

            if (isExt4) {
                if (f2fsGcInfoContainer) f2fsGcInfoContainer.style.display = 'none';
                if (f2fsGcConfigContainer) f2fsGcConfigContainer.style.display = 'none';
                if (gcInfoIntervalId) {
                    clearInterval(gcInfoIntervalId);
                    gcInfoIntervalId = null;
                }
            } else {
                if (f2fsGcInfoContainer) f2fsGcInfoContainer.style.display = 'block';
                if (f2fsGcConfigContainer) f2fsGcConfigContainer.style.display = 'block';
                if (!gcInfoIntervalId) {
                    gcInfoIntervalId = setInterval(updateAllF2fsInfo, 1000);
                }
            }
        } catch (error) {
            toast(`检查文件系统失败: ${error.message}`);
            console.error("检查文件系统失败:", error);
            if (f2fsGcInfoContainer) f2fsGcInfoContainer.style.display = 'none';
            if (f2fsGcConfigContainer) f2fsGcConfigContainer.style.display = 'none';
            if (gcInfoIntervalId) {
                clearInterval(gcInfoIntervalId);
                gcInfoIntervalId = null;
            }
        }
        return fsType;
    }

    async function updateAllF2fsInfo() {
        const info = await getRealtimeF2fsInfo();
        if (!info) {
            gcStatusSpan.textContent = '状态\n错误';
            gcStatusSpan.className = 'badge bg-danger';
            gcControlButton.textContent = '状态未知';
            gcControlButton.className = 'btn btn-sm btn-outline-secondary';
            gcControlButton.dataset.action = 'unknown';
            return;
        }
        updateSegmentInfo(info);
        updateGcControlButton(info);
    }

    function updateSegmentInfo(info) {
        if (isExt4 || !info) return;
        
        const dirty = parseInt(info.dirty_segments, 10);
        const free = parseInt(info.free_segments, 10);

        if (isNaN(dirty) || isNaN(free)) {
            gcStatusSpan.textContent = '状态\n数据无效';
            gcStatusSpan.className = 'badge bg-warning';
            return;
        }

        if (dirty !== lastChartData.dirty || free !== lastChartData.free) {
            segmentChart.data.labels = [`脏段 (${dirty})`, `空闲段 (${free})`];
            segmentChart.data.datasets[0].data = [dirty, free];
            segmentChart.update();
            lastChartData.dirty = dirty;
            lastChartData.free = free;
        }
        
        const gcStatusValue = info.gc_urgent;
        if (gcStatusValue === '0' || gcStatusValue.includes('GC_NORMAL')) {
            gcStatusSpan.textContent = 'GC回收\n关闭';
            gcStatusSpan.className = 'badge bg-secondary';
        } else if (gcStatusValue === '1' || gcStatusValue.includes('GC_URGENT_HIGH') || gcStatusValue === '4') {
            gcStatusSpan.textContent = 'GC回收\n开启';
            gcStatusSpan.className = 'badge bg-success';
        } else {
            gcStatusSpan.textContent = '状态\n未知';
            gcStatusSpan.className = 'badge bg-secondary';
        }
    }

    function updateGcControlButton(info) {
        if (isExt4 || !info) return;
        
        const gcStatusValue = info.gc_urgent;
        const isActive = gcStatusValue === '1' || gcStatusValue === '4' || gcStatusValue.includes('GC_URGENT_HIGH');

        if (isActive) {
            gcControlButton.textContent = '停止';
            gcControlButton.className = 'btn btn-sm btn-danger';
            gcControlButton.dataset.action = 'stop';
        } else {
            gcControlButton.textContent = '开始';
            gcControlButton.className = 'btn btn-sm btn-success';
            gcControlButton.dataset.action = 'start';
        }
    }

    // --- 配置表单处理 ---
    configForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const retentionDays = retentionDaysInput.value;
        const cleanInterval = cleanIntervalInput.value;
        const f2fsGcEnabled = f2fsGcConfigToggle.checked ? 'y' : 'n';
        await saveConfigFile(retentionDays, cleanInterval, f2fsGcEnabled);
        toast('配置已保存，请重启模块以应用所有更改。');
    });

    async function loadConfigFile() {
        try {
            const { errno, stdout, stderr } = await exec('cat /data/media/0/Android/清理规则/配置.txt');
            if (errno === 0) {
                const config = parseConfig(stdout);
                retentionDaysInput.value = config.保留天数 || '30';
                cleanIntervalInput.value = config.程序清理间隔秒数 || '3600';
                const f2fsGcValue = config['f2fs-GC'] || 'n';
                f2fsGcConfigToggle.checked = f2fsGcValue === 'y';
                updateGcConfigToggleLabel(f2fsGcConfigToggle.checked);
            } else {
                toast(`错误: ${stderr}`);
                console.error("加载配置失败:", stderr);
            }
        } catch (error) {
            toast(`加载配置失败: ${error.message}`);
            console.error("加载配置失败:", error);
        }
    }

    function parseConfig(text) {
        const config = {};
        text.split('\n').forEach(line => {
            if (line.includes('=')) {
                const [key, value] = line.split('=').map(item => item.trim());
                if (key && value) config[key] = value;
            }
        });
        return config;
    }

    async function saveConfigFile(retentionDays, cleanInterval, f2fsGcEnabled) {
        try {
            const { errno, stdout, stderr } = await exec('cat /data/media/0/Android/清理规则/配置.txt');
            let lines = [];
            if (errno === 0) {
                lines = stdout.split('\n');
            } else if (!stderr.includes('No such file or directory')) {
                toast(`读取配置文件失败: ${stderr}`);
                console.error("读取配置文件失败:", stderr);
                return;
            }

            let hasRetention = false, hasInterval = false, hasF2fsGc = false;
            lines = lines.map(line => {
                if (line.startsWith('保留天数=')) { hasRetention = true; return `保留天数=${retentionDays}`; }
                if (line.startsWith('程序清理间隔秒数=')) { hasInterval = true; return `程序清理间隔秒数=${cleanInterval}`; }
                if (line.startsWith('f2fs-GC=')) { hasF2fsGc = true; return `f2fs-GC=${f2fsGcEnabled}`; }
                return line;
            });
            if (!hasRetention) lines.push(`保留天数=${retentionDays}`);
            if (!hasInterval) lines.push(`程序清理间隔秒数=${cleanInterval}`);
            if (!hasF2fsGc) lines.push(`f2fs-GC=${f2fsGcEnabled}`);
            
            const updatedConfig = lines.filter(line => line.trim() !== '').join('\n');
            const command = `printf "%s" "${updatedConfig}" > /data/media/0/Android/清理规则/配置.txt`;
            const { errno: writeErrno, stderr: writeStderr } = await exec(command);
            if (writeErrno !== 0) {
                toast(`错误: ${writeStderr}`);
                console.error("写入配置文件失败:", writeStderr);
            }
        } catch (error) {
            toast(`保存配置失败: ${error.message}`);
            console.error("保存配置失败:", error);
        }
    }

    function updateGcConfigToggleLabel(isChecked) {
        if (isChecked) {
            f2fsGcConfigToggleLabel.textContent = '已开启';
            f2fsGcConfigToggleLabel.classList.remove('btn-outline-secondary');
            f2fsGcConfigToggleLabel.classList.add('btn-success');
        } else {
            f2fsGcConfigToggleLabel.textContent = '已关闭';
            f2fsGcConfigToggleLabel.classList.remove('btn-success');
            f2fsGcConfigToggleLabel.classList.add('btn-outline-secondary');
        }
    }

    f2fsGcConfigToggle.addEventListener('change', () => {
        updateGcConfigToggleLabel(f2fsGcConfigToggle.checked);
    });

    // --- 图表初始化与更新 ---
    const segmentChartCtx = document.getElementById('segment-chart').getContext('2d');
    const isDarkMode = window.matchMedia('(prefers-color-scheme: dark)').matches;
    const segmentChart = new Chart(segmentChartCtx, {
        type: 'doughnut',
        data: {
            labels: ['脏段 (0)', '空闲段 (0)'],
            datasets: [{ data: [0, 0], backgroundColor: ['#ff6384', '#36a2eb'], borderColor: isDarkMode ? '#2A2A2A' : '#f8f9fa', borderWidth: 1, }],
        },
        options: {
            responsive: true, maintainAspectRatio: false,
            plugins: {
                legend: { position: 'bottom', labels: { color: isDarkMode ? '#f8f9fa' : '#2A2A2A' } },
                tooltip: { enabled: true, backgroundColor: isDarkMode ? '#333333' : '#f8f9fa', titleColor: isDarkMode ? '#f8f9fa' : '#2A2A2A', bodyColor: isDarkMode ? '#f8f9fa' : '#2A2A2A' },
            },
        },
    });

    const barChartCtx = document.getElementById('bar-chart').getContext('2d');
    const barChart = new Chart(barChartCtx, {
        type: 'bar',
        data: {
            labels: [],
            datasets: [
                { label: '已删除文件数', data: [], backgroundColor: 'rgba(255, 99, 132, 0.2)', borderColor: 'rgba(255, 99, 132, 1)', borderWidth: 1 },
                { label: '已删除目录数', data: [], backgroundColor: 'rgba(54, 162, 235, 0.2)', borderColor: 'rgba(54, 162, 235, 1)', borderWidth: 1 },
                { label: '脏段', data: [], backgroundColor: 'rgba(75, 192, 192, 0.2)', borderColor: 'rgba(75, 192, 192, 1)', borderWidth: 1, hidden: isExt4 },
                { label: '已删除垃圾 (MB)', data: [], backgroundColor: 'rgba(153, 102, 255, 0.2)', borderColor: 'rgba(153, 102, 255, 1)', borderWidth: 1 },
            ],
        },
        options: { scales: { y: { beginAtZero: true } } },
    });

    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', (e) => {
        updateChartTheme(e.matches);
    });

    function updateChartTheme(isDarkMode) {
        segmentChart.data.datasets[0].borderColor = isDarkMode ? '#2A2A2A' : '#f8f9fa';
        segmentChart.options.plugins.legend.labels.color = isDarkMode ? '#f8f9fa' : '#2A2A2A';
        segmentChart.options.plugins.tooltip.backgroundColor = isDarkMode ? '#333333' : '#f8f9fa';
        segmentChart.options.plugins.tooltip.titleColor = isDarkMode ? '#f8f9fa' : '#2A2A2A';
        segmentChart.options.plugins.tooltip.bodyColor = isDarkMode ? '#f8f9fa' : '#2A2A2A';
        segmentChart.update();
        barChart.options.scales.y.ticks.color = isDarkMode ? '#f8f9fa' : '#2A2A2A';
        barChart.options.scales.x.ticks.color = isDarkMode ? '#f8f9fa' : '#2A2A2A';
        barChart.options.plugins.legend.labels.color = isDarkMode ? '#f8f9fa' : '#2A2A2A';
        barChart.options.plugins.tooltip.backgroundColor = isDarkMode ? '#333333' : '#f8f9fa';
        barChart.options.plugins.tooltip.titleColor = isDarkMode ? '#f8f9fa' : '#2A2A2A';
        barChart.options.plugins.tooltip.bodyColor = isDarkMode ? '#f8f9fa' : '#2A2A2A';
        barChart.update();
    }

    // --- 日期选择器和日志数据处理 ---
    async function getCurrentDate() {
        try {
            const { errno, stdout } = await exec('date +"%F"');
            return errno === 0 ? stdout.trim() : null;
        } catch (error) {
            toast(`获取当前日期失败: ${error.message}`);
            console.error("获取当前日期失败:", error);
            return null;
        }
    }

    async function initDatePicker() {
        try {
            const currentDateStr = await getCurrentDate();
            if (!currentDateStr) throw new Error('无法获取当前日期');
            const today = new Date(currentDateStr);
            const sixDaysAgo = new Date(today);
            sixDaysAgo.setDate(today.getDate() - 6);
            const formatDate = (date) => date.toISOString().split('T')[0];
            dateSelect.min = formatDate(sixDaysAgo);
            dateSelect.max = formatDate(today);
            dateSelect.value = formatDate(today);
            dateSelect.addEventListener('change', updateDisplaysForSelectedDate);
        } catch (error) {
            toast(`初始化日期选择器失败: ${error.message}`);
            console.error("初始化日期选择器失败:", error);
        }
    }

    async function loadLogFile() {
        try {
            const { errno, stdout, stderr } = await exec('cat /data/adb/modules/Clean-C/stats.json');
            if (errno === 0 && stdout.trim() !== '') {
                const parsedData = parseLogContent(stdout);
                updateLocalStorage(parsedData);
                // --- MODIFICATION START ---
                // The automatic deletion of stats.json after parsing has been removed as requested.
                // The file will now persist until manually deleted via the "Delete Log" button.
                /*
                try {
                    await exec('rm -f /data/adb/modules/Clean-C/stats.json');
                } catch (e) {
                    console.warn("删除 stats.json 失败 (可能文件不存在或权限问题):", e);
                }
                */
                // --- MODIFICATION END ---
            } else if (errno !== 0 && !stderr.includes('No such file or directory')) {
                throw new Error(`读取统计文件失败: ${stderr}`);
            }
        } catch (error) {
            toast(`加载统计数据失败: ${error.message}`);
            console.error("加载统计数据失败:", error);
        }
        updateDisplaysForSelectedDate();
    }

    function aggregateAppStatsForDate(selectedDate) {
        const storedData = getStoredData();
        const dailyEntries = storedData.filter(entry => entry.date === selectedDate);
        const aggregatedStats = {};
        dailyEntries.forEach(entry => {
            if (entry.appStats) {
                entry.appStats.forEach(app => {
                    if (aggregatedStats[app.package_name]) {
                        aggregatedStats[app.package_name].bytes_deleted += app.bytes_deleted;
                        aggregatedStats[app.package_name].megabytes_deleted += app.megabytes_deleted;
                    } else {
                        aggregatedStats[app.package_name] = { ...app };
                    }
                });
            }
        });
        return Object.values(aggregatedStats);
    }

    function updateAppStatsList(appStats) {
        appStatsList.innerHTML = '';
        if (!appStats || appStats.length === 0) {
            const li = document.createElement('li');
            li.className = 'list-group-item text-muted';
            li.textContent = '该日无应用数据清理记录。';
            appStatsList.appendChild(li);
            return;
        }
        appStats.sort((a, b) => b.bytes_deleted - a.bytes_deleted);
        appStats.forEach(app => {
            const li = document.createElement('li');
            li.className = 'list-group-item d-flex justify-content-between align-items-center';
            const packageNameSpan = document.createElement('span');
            packageNameSpan.textContent = app.package_name;
            packageNameSpan.className = 'text-truncate me-3';
            const sizeBadge = document.createElement('span');
            sizeBadge.className = 'badge bg-primary rounded-pill';
            sizeBadge.textContent = `${app.megabytes_deleted.toFixed(2)} MB`;
            li.appendChild(packageNameSpan);
            li.appendChild(sizeBadge);
            appStatsList.appendChild(li);
        });
    }

    function updateBarChart() {
        const storedData = getStoredData();
        const selectedDate = dateSelect.value;
        const filteredData = selectedDate ? storedData.filter(entry => entry.date === selectedDate) : storedData;
        
        const aggregatedData = {};
        filteredData.forEach(entry => {
            const date = entry.date;
            if (!aggregatedData[date]) {
                aggregatedData[date] = { deletedFiles: 0, deletedDirs: 0, dirtySegments: 0, trimmedMB: 0 };
            }
            aggregatedData[date].deletedFiles += entry.deletedFiles;
            aggregatedData[date].deletedDirs += entry.deletedDirs;
            if (!isExt4) {
                aggregatedData[date].dirtySegments += entry.dirtySegments;
            }
            aggregatedData[date].trimmedMB += entry.trimmedMB;
        });

        barChart.data.datasets.forEach(dataset => {
            if (dataset.label === '脏段') {
                dataset.hidden = isExt4;
            }
        });

        const dates = Object.keys(aggregatedData).sort();
        barChart.data.labels = dates;
        barChart.data.datasets[0].data = dates.map(date => aggregatedData[date].deletedFiles);
        barChart.data.datasets[1].data = dates.map(date => aggregatedData[date].deletedDirs);
        if (!isExt4) {
            barChart.data.datasets[2].data = dates.map(date => aggregatedData[date].dirtySegments);
        }
        barChart.data.datasets[3].data = dates.map(date => aggregatedData[date].trimmedMB);
        barChart.update();
    }

    function updateDisplaysForSelectedDate() {
        const selectedDate = dateSelect.value;
        updateBarChart();
        appStatsTitle.textContent = `应用清理详情 (${selectedDate})`;
        const aggregatedData = aggregateAppStatsForDate(selectedDate);
        updateAppStatsList(aggregatedData);
    }

    // --- 事件监听器 ---
    clearDataBtn.addEventListener('click', () => {
        clearStoredData();
        updateDisplaysForSelectedDate();
        toast('数据已清除');
    });

    gcControlButton.addEventListener('click', async () => {
        const action = gcControlButton.dataset.action;
        if (action === 'unknown') {
            toast('无法确定GC状态，请刷新。');
            return;
        }

        gcControlButton.disabled = true;
        gcControlButton.textContent = '...';

        if (action === 'start') {
            await sendUdpCommand('start_gc');
        } else {
            await sendUdpCommand('stop_gc');
        }

        await delay(1500);
        gcControlButton.disabled = false;
        await updateAllF2fsInfo();
    });

    if (cleanNowBtn) {
        cleanNowBtn.addEventListener('click', async () => {
            toast('正在请求立即清理...');
            await sendUdpCommand('clean_now');
        });
    }

    editBlacklist1Btn.addEventListener('click', () => editRuleFile('blacklist1.txt'));
    editBlacklist2Btn.addEventListener('click', () => editRuleFile('blacklist2.txt'));
    editWhitelistBtn.addEventListener('click', () => editRuleFile('whitelist.txt'));

    async function editRuleFile(fileName) {
        try {
            const filePath = `/data/media/0/Android/清理规则/${fileName}`;
            const { errno, stderr } = await exec(`am start -a android.intent.action.VIEW -d file://${filePath} -t text/plain`);
            if (errno !== 0) {
                throw new Error(`编辑文件失败: ${stderr}`);
            }
            toast(`尝试打开文件: ${fileName}`);
        } catch (error) {
            toast(`编辑文件失败: ${error.message}`);
            console.error("编辑文件失败:", error);
        }
    }

    refreshLogBtn.addEventListener('click', async () => {
        await loadLogFile();
        if (!isExt4) {
            await updateAllF2fsInfo();
        }
        toast('数据已刷新');
    });

    deleteLogBtn.addEventListener('click', async () => {
        try {
            const { errno, stderr } = await exec('rm -f /data/adb/modules/Clean-C/run.log /data/adb/modules/Clean-C/stats.json');
            if (errno === 0) {
                await loadLogFile();
                toast('日志文件已删除');
            } else {
                throw new Error(`删除日志失败: ${stderr}`);
            }
        } catch (error) {
            toast(`删除日志失败: ${error.message}`);
            console.error("删除日志失败:", error);
        }
    });

    restartModuleBtn.addEventListener('click', async () => {
        try {
            const { errno, stderr } = await exec('sh /data/adb/modules/Clean-C/rest.sh');
            if (stderr) toast(`stderr: ${stderr}`);
            if (errno === 0) toast('模块已重启');
            else toast(`重启模块失败: ${stderr || '未知错误'}`);
        } catch (error) {
            toast(`模块重启失败: ${error.message}`);
            console.error("模块重启失败:", error);
        }
    });

    // --- 页面初始化 ---
    await checkFileSystem();
    await initDatePicker();
    await loadConfigFile();
    await loadLogFile();
    if (!isExt4) {
        await updateAllF2fsInfo();
    }
});