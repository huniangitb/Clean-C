import { exec, toast } from 'kernelsu';

document.addEventListener('DOMContentLoaded', async () => {
    const logContent = document.getElementById('log-content');
    const configForm = document.getElementById('config-form');
    const retentionDaysInput = document.getElementById('retention-days');
    const cleanIntervalInput = document.getElementById('clean-interval');
    const editBlacklist1Btn = document.getElementById('edit-blacklist1');
    const editBlacklist2Btn = document.getElementById('edit-blacklist2');
    const editWhitelistBtn = document.getElementById('edit-whitelist');
    const refreshLogBtn = document.getElementById('refresh-log');
    const deleteLogBtn = document.getElementById('delete-log');
    const restartModuleBtn = document.getElementById('restart-module');
    const f2fsGcToggle = document.getElementById('f2fs-gc-toggle');
    const f2fsGcToggleLabel = document.getElementById('f2fs-gc-toggle-label');
    const gcStatusSpan = document.getElementById('gc-status');

    // 初始化 Chart.js 环形图
    const segmentChartCtx = document.getElementById('segment-chart').getContext('2d');

    // 检测深色模式
    const isDarkMode = window.matchMedia('(prefers-color-scheme: dark)').matches;

    // 根据深色模式设置环形图的边框颜色
    const segmentChart = new Chart(segmentChartCtx, {
        type: 'doughnut', // 环形图
        data: {
            labels: ['脏段 (0)', '空闲段 (0)'], // 初始标签
            datasets: [{
                data: [0, 0],
                backgroundColor: ['#ff6384', '#36a2eb'],
                borderColor: isDarkMode ? '#2A2A2A' : '#f8f9fa', // 根据深色模式设置边框颜色
                borderWidth: 1, // 设置边框宽度
            }],
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            plugins: {
                legend: {
                    position: 'bottom', // 图例位置
                    labels: {
                        color: isDarkMode ? '#f8f9fa' : '#2A2A2A', // 根据深色模式设置图例文字颜色
                    },
                },
                tooltip: {
                    enabled: true, // 启用提示
                    backgroundColor: isDarkMode ? '#333333' : '#f8f9fa', // 根据深色模式设置提示框背景色
                    titleColor: isDarkMode ? '#f8f9fa' : '#2A2A2A', // 根据深色模式设置提示框标题颜色
                    bodyColor: isDarkMode ? '#f8f9fa' : '#2A2A2A', // 根据深色模式设置提示框内容颜色
                },
            },
        },
    });

    // 监听系统主题变化
    window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', (e) => {
        const isDarkMode = e.matches;
        updateChartTheme(isDarkMode);
    });

    // 更新图表主题
    function updateChartTheme(isDarkMode) {
        segmentChart.data.datasets[0].borderColor = isDarkMode ? '#2A2A2A' : '#f8f9fa'; // 更新边框颜色
        segmentChart.options.plugins.legend.labels.color = isDarkMode ? '#f8f9fa' : '#2A2A2A'; // 更新图例文字颜色
        segmentChart.options.plugins.tooltip.backgroundColor = isDarkMode ? '#333333' : '#f8f9fa'; // 更新提示框背景色
        segmentChart.options.plugins.tooltip.titleColor = isDarkMode ? '#f8f9fa' : '#2A2A2A'; // 更新提示框标题颜色
        segmentChart.options.plugins.tooltip.bodyColor = isDarkMode ? '#f8f9fa' : '#2A2A2A'; // 更新提示框内容颜色
        segmentChart.update(); // 更新图表
    }

    // 加载日志文件
    async function loadLogFile() {
        try {
            const { errno, stdout, stderr } = await exec('cat /data/adb/modules/Clean-C/log.txt');
            if (errno === 0) {
                logContent.innerHTML = highlightNumbers(stdout);
                logContent.scrollTop = logContent.scrollHeight;
            } else {
                toast(`错误: ${stderr}`);
            }
        } catch (error) {
            toast(`加载日志失败: ${error.message}`);
        }
    }

    // 高亮显示数字
    function highlightNumbers(text) {
        return text.replace(/\b\d+\b/g, '<span class="highlight">$&</span>');
    }

    // 加载配置文件
    async function loadConfigFile() {
        try {
            const { errno, stdout, stderr } = await exec('cat /storage/emulated/0/Android/清理规则/配置.txt');
            if (errno === 0) {
                const config = parseConfig(stdout);
                retentionDaysInput.value = config.retentionDays || 30; // 默认值 30
                cleanIntervalInput.value = config.cleanInterval || 3600; // 默认值 3600

                // 设置 f2fs垃圾回收 开关状态
                if (config['f2fs-GC'] === 'y') {
                    f2fsGcToggle.checked = true;
                    f2fsGcToggleLabel.textContent = '已开启';
                    f2fsGcToggleLabel.classList.remove('btn-outline-secondary');
                    f2fsGcToggleLabel.classList.add('btn-success'); // 绿色
                } else {
                    f2fsGcToggle.checked = false;
                    f2fsGcToggleLabel.textContent = '关闭';
                    f2fsGcToggleLabel.classList.remove('btn-success');
                    f2fsGcToggleLabel.classList.add('btn-outline-secondary'); // 灰色
                }
            } else {
                toast(`错误: ${stderr}`);
            }
        } catch (error) {
            toast(`加载配置失败: ${error.message}`);
        }
    }

    // 解析配置文件
    function parseConfig(text) {
        const lines = text.split('\n');
        const config = {};
        lines.forEach(line => {
            const [key, value] = line.split('=');
            if (key && value) {
                config[key.trim()] = value.trim();
            }
        });
        return config;
    }

    // 保存配置文件
    async function saveConfigFile(retentionDays, cleanInterval) {
        try {
            // 读取配置文件内容
            const { errno: readErrno, stdout, stderr: readStderr } = await exec('cat /storage/emulated/0/Android/清理规则/配置.txt');
            if (readErrno !== 0) {
                toast(`读取配置文件失败: ${readStderr}`);
                return;
            }

            // 更新配置内容
            const lines = stdout.split('\n');
            const updatedLines = lines.map(line => {
                if (line.startsWith('保留天数=')) {
                    return `保留天数=${retentionDays}`;
                } else if (line.startsWith('程序清理间隔秒数=')) {
                    return `程序清理间隔秒数=${cleanInterval}`;
                }
                return line;
            });

            // 写入更新后的配置
            const updatedConfig = updatedLines.join('\n');
            const { errno: writeErrno, stderr: writeStderr } = await exec(`echo "${updatedConfig}" > /storage/emulated/0/Android/清理规则/配置.txt`);
            if (writeErrno === 0) {
                toast('配置保存成功');
            } else {
                toast(`错误: ${writeStderr}`);
            }
        } catch (error) {
            toast(`保存配置失败: ${error.message}`);
        }
    }

    // 设置 f2fs垃圾回收 参数
    async function setF2fsGc(value) {
        try {
            // 读取配置文件内容
            const { errno: readErrno, stdout, stderr: readStderr } = await exec('cat /storage/emulated/0/Android/清理规则/配置.txt');
            if (readErrno !== 0) {
                toast(`读取配置文件失败: ${readStderr}`);
                return;
            }

            // 更新配置内容
            const lines = stdout.split('\n');
            let f2fsGcFound = false;
            const updatedLines = lines.map(line => {
                if (line.startsWith('f2fs-GC=')) {
                    f2fsGcFound = true;
                    return `f2fs-GC=${value}`;
                }
                return line;
            });

            // 如果 f2fs-GC 参数不存在，则添加
            if (!f2fsGcFound) {
                updatedLines.push(`f2fs-GC=${value}`);
            }

            // 写入更新后的配置
            const updatedConfig = updatedLines.join('\n');
            const { errno: writeErrno, stderr: writeStderr } = await exec(`echo "${updatedConfig}" > /storage/emulated/0/Android/清理规则/配置.txt`);
            if (writeErrno === 0) {
                toast(`f2fs垃圾回收 已设置为 ${value}`);
            } else {
                toast(`错误: ${writeStderr}`);
            }
        } catch (error) {
            toast(`设置 f2fs垃圾回收 失败: ${error.message}`);
        }
    }

    // 编辑规则文件
    async function editRuleFile(fileName) {
        try {
            const filePath = `/storage/emulated/0/Android/清理规则/${fileName}`;
            const { errno, stderr } = await exec(`am start -a android.intent.action.VIEW -d "file://${filePath}" -t "text/plain"`);
            if (errno !== 0) {
                toast(`错误: ${stderr}`);
            }
        } catch (error) {
            toast(`编辑文件失败: ${error.message}`);
        }
    }

    // 刷新日志
    refreshLogBtn.addEventListener('click', async () => {
        await loadLogFile();
        toast('日志已刷新');
    });

    // 删除日志
    deleteLogBtn.addEventListener('click', async () => {
        try {
            const { errno, stderr } = await exec('echo "" > /data/adb/modules/Clean-C/log.txt');
            if (errno === 0) {
                await loadLogFile();
                toast('日志已删除');
            } else {
                toast(`错误: ${stderr}`);
            }
        } catch (error) {
            toast(`删除日志失败: ${error.message}`);
        }
    });

    // 重启模块
    restartModuleBtn.addEventListener('click', async () => {
        try {
            const { errno, stderr } = await exec('/data/adb/modules/Clean-C/kill.sh');
            if (errno === 0) {
                toast('模块已重启');
            } else {
                toast(`错误: ${stderr}`);
            }
        } catch (error) {
            toast(`重启模块失败: ${error.message}`);
        }
    });

    // 获取并更新脏段和空闲段信息
    async function updateSegmentInfo() {
        try {
            const DATA_DEVICE = (await exec('getprop dev.mnt.dev.data')).stdout.trim();
            const F2FS_SYSFS = `/sys/fs/f2fs/${DATA_DEVICE}`;

            const initial_dirty_segs = (await exec(`cat ${F2FS_SYSFS}/dirty_segments`)).stdout.trim();
            const current_free_segs = (await exec(`cat ${F2FS_SYSFS}/free_segments`)).stdout.trim();

            // 更新环形图数据和标签
            segmentChart.data.labels = [
                `脏段 (${initial_dirty_segs})`,
                `空闲段 (${current_free_segs})`,
            ];
            segmentChart.data.datasets[0].data = [initial_dirty_segs, current_free_segs];
            segmentChart.update();

            // 获取 GC 状态
            const gcUrgent = (await exec(`cat ${F2FS_SYSFS}/gc_urgent`)).stdout.trim();
            if (gcUrgent.includes('GC_NORMAL')) {
                gcStatusSpan.textContent = 'GC回收关闭';
            } else if (gcUrgent.includes('GC_URGENT_HIGH')) {
                gcStatusSpan.textContent = 'GC回收开启';
            } else {
                gcStatusSpan.textContent = '未知';
            }
        } catch (error) {
            toast(`获取段信息失败: ${error.message}`);
        }
    }

    // 表单提交事件
    configForm.addEventListener('submit', async (e) => {
        e.preventDefault();
        const retentionDays = retentionDaysInput.value;
        const cleanInterval = cleanIntervalInput.value;
        await saveConfigFile(retentionDays, cleanInterval);
    });

    // 绑定按钮事件
    editBlacklist1Btn.addEventListener('click', () => editRuleFile('blacklist1.txt'));
    editBlacklist2Btn.addEventListener('click', () => editRuleFile('blacklist2.txt'));
    editWhitelistBtn.addEventListener('click', () => editRuleFile('whitelist.txt'));

    // 设置 f2fs垃圾回收 开关状态
    f2fsGcToggle.addEventListener('change', async () => {
        const value = f2fsGcToggle.checked ? 'y' : 'n';
        await setF2fsGc(value);

        // 更新按钮样式和文字
        if (value === 'y') {
            f2fsGcToggleLabel.textContent = '已开启';
            f2fsGcToggleLabel.classList.remove('btn-outline-secondary');
            f2fsGcToggleLabel.classList.add('btn-success'); // 绿色
        } else {
            f2fsGcToggleLabel.textContent = '关闭';
            f2fsGcToggleLabel.classList.remove('btn-success');
            f2fsGcToggleLabel.classList.add('btn-outline-secondary'); // 灰色
        }
    });

    // 初始化
    await loadLogFile();
    await loadConfigFile();
    await updateSegmentInfo(); // 初始化段信息
});