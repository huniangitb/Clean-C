import { exec， spawn， toast } from 'kernelsu';
import { parseLogContent， updateLocalStorage， getStoredData， clearStoredData } from './logParser.js';
import { Ripple， initMDB } from 'mdb-ui-kit/js/mdb.es.min.js';
import Chart from 'chart.js/auto';
window。Ripple = Ripple;

initMDB({ Ripple });

document。addEventListener('DOMContentLoaded'， async () => {
    // 全局变量声明
    let isExt4 = false;
    const configForm = document。getElementById('config-form');
    const retentionDaysInput = document。getElementById('retention-days');
    const cleanIntervalInput = document。getElementById('clean-interval');
    const editBlacklist1Btn = document。getElementById('edit-blacklist1');
    const editBlacklist2Btn = document。getElementById('edit-blacklist2');
    const editWhitelistBtn = document。getElementById('edit-whitelist');
    const refreshLogBtn = document。getElementById('refresh-log');
    const deleteLogBtn = document。getElementById('delete-log');
    const clearDataBtn = document。getElementById('clear-data');
    const restartModuleBtn = document。getElementById('restart-module');
    const f2fsGcToggle = document。getElementById('f2fs-gc-toggle');
    const f2fsGcToggleLabel = document。getElementById('f2fs-gc-toggle-label');
    const gcStatusSpan = document。getElementById('gc-status');
    const dateSelect = document。getElementById('date-select');
    const segmentChartContainer = document。getElementById('segment-chart')。parentElement;
    const f2fsGcContainer = document。getElementById('f2fs-gc-container');
    const gcControlLabel = document。getElementById('gc-control-label');

    // 检查文件系统类型并更新UI
    async function checkFileSystem() {
        let fsType = null;

        try {
            const { errno， stdout， stderr } = await exec(`mount | grep " /data " | awk '{print $5}'`);
            if (errno === 0) {
                fsType = stdout。trim();
                isExt4 = fsType === 'ext4';
            } else {
                throw new 错误(stderr || '执行命令失败');
            }

            // 更新元素显示状态
            if (segmentChartContainer && f2fsGcContainer && gcStatusSpan) {
                segmentChartContainer。style。display = isExt4 ? 'none' : 'block';
                f2fsGcContainer。style。display = 'block';
                gcStatusSpan。closest('p')。style。display = isExt4 ? 'none' : 'block';

                // 更新标签和样式
                f2fsGcToggleLabel。textContent = isExt4 ? 'EXT4碎片整理' : 'f2fs垃圾回收';
                f2fsGcToggleLabel。classList。toggle('btn-outline-secondary', !isExt4);
                f2fsGcToggleLabel。classList。toggle('btn-success'， isExt4);
            }
            gcControlLabel。textContent = isExt4 ? 'EXT4碎片整理' : 'f2fs垃圾回收';

        } catch (error) {
            toast(`检查文件系统失败: ${error。message}`);
        }

        return fsType;
    }

    // 表单提交事件
    configForm。addEventListener('submit'， async (e) => {
        e。preventDefault();
        const retentionDays = retentionDaysInput。value;
        const cleanInterval = cleanIntervalInput。value;
        await saveConfigFile(retentionDays， cleanInterval);
    });

    // 初始化 Chart.js 环形图
    const segmentChartCtx = document。getElementById('segment-chart')。getContext('2d');

    // 检测深色模式
    const isDarkMode = window。matchMedia('(prefers-color-scheme: dark)')。matches;

    // 根据深色模式设置环形图的边框颜色
    const segmentChart = new Chart(segmentChartCtx， {
        type: 'doughnut'，
        data: {
            labels: ['脏段 (0)'， '空闲段 (0)']，
            datasets: [{
                data: [0， 0]，
                backgroundColor: ['#ff6384'， '#36a2eb']，
                borderColor: isDarkMode ? '#2A2A2A' : '#f8f9fa'，
                borderWidth: 1，
            }]，
        }，
        options: {
            responsive: true，
            maintainAspectRatio: false，
            plugins: {
                legend: {
                    position: 'bottom'，
                    labels: {
                        color: isDarkMode ? '#f8f9fa' : '#2A2A2A'，
                    }，
                }，
                tooltip: {
                    enabled: true，
                    backgroundColor: isDarkMode ? '#333333' : '#f8f9fa'，
                    titleColor: isDarkMode ? '#f8f9fa' : '#2A2A2A'，
                    bodyColor: isDarkMode ? '#f8f9fa' : '#2A2A2A'，
                }，
            }，
        }，
    });

    // 初始化柱状图 (移除裁剪数据)
    const barChartCtx = document。getElementById('bar-chart')。getContext('2d');
    const barChart = new Chart(barChartCtx， {
        type: 'bar'，
        data: {
            labels: []，
            datasets: [
                {
                    label: '已删除文件数'，
                    data: []，
                    backgroundColor: 'rgba(255, 99, 132, 0.2)'，
                    borderColor: 'rgba(255, 99, 132, 1)'，
                    borderWidth: 1，
                }，
                {
                    label: '已删除目录数'，
                    data: []，
                    backgroundColor: 'rgba(54, 162, 235, 0.2)'，
                    borderColor: 'rgba(54, 162, 235, 1)'，
                    borderWidth: 1，
                }，
                ...(!isExt4 ? [{
                    label: '脏段'，
                    data: []，
                    backgroundColor: 'rgba(75, 192, 192, 0.2)'，
                    borderColor: 'rgba(75, 192, 192, 1)'，
                    borderWidth: 1，
                }] : [])，
                // 移除裁剪数据
            ]，
        }，
        options: {
            scales: {
                y: {
                    beginAtZero: true，
                }，
            }，
        }，
    });

    // 监听系统主题变化
    window。matchMedia('(prefers-color-scheme: dark)')。addEventListener('change'， (e) => {
        const isDarkMode = e。matches;
        updateChartTheme(isDarkMode);
    });

    // 更新图表主题
    function updateChartTheme(isDarkMode) {
        segmentChart。data。datasets[0]。borderColor = isDarkMode ? '#2A2A2A' : '#f8f9fa';
        segmentChart。options。plugins。legend。labels。color = isDarkMode ? '#f8f9fa' : '#2A2A2A';
        segmentChart。options。plugins。tooltip。backgroundColor = isDarkMode ? '#333333' : '#f8f9fa';
        segmentChart。options。plugins。tooltip。titleColor = isDarkMode ? '#f8f9fa' : '#2A2A2A';
        segmentChart。options。plugins。tooltip。bodyColor = isDarkMode ? '#f8f9fa' : '#2A2A2A';
        segmentChart。update();
    }

    // 初始化日期选择器
    async function initDatePicker() {
        try {
            const currentDateStr = await getCurrentDate();
            if (!currentDateStr) {
                throw new 错误('无法获取当前日期');
            }

            const today = new Date(currentDateStr);
            const sixDaysAgo = new Date(today);
            sixDaysAgo。setDate(today。getDate() - 6);

            const formatDate = (date) => {
                const year = date。getFullYear();
                const month = String(date。getMonth() + 1)。padStart(2， '0');
                const day = String(date。getDate())。padStart(2， '0');
                return `${year}-${month}-${day}`;
            };

            dateSelect。min = formatDate(sixDaysAgo);
            dateSelect。max = formatDate(today);
            dateSelect。value = formatDate(today);

            dateSelect。addEventListener('change'， () => {
                updateBarChart();
            });
        } catch (error) {
            toast(`初始化日期选择器失败: ${error。message}`);
        }
    }

    async function getCurrentDate() {
        try {
            const { errno， stdout， stderr } = await exec('date +"%F"');
            if (errno === 0) {
                return stdout。trim();
            } else {
                throw new 错误(`执行 date 命令失败: ${stderr}`);
            }
        } catch (error) {
            toast(`获取当前日期失败: ${error。message}`);
            return null;
        }
    }

    // 加载日志文件
    async function loadLogFile() {
        try {
            const { errno， stdout， stderr } = await exec('cat /data/adb/modules/Clean-C/run.log');
            if (errno === 0) {
                const parsedData = parseLogContent(stdout);
                updateLocalStorage(parsedData);
                updateBarChart();
            } else {
                throw new 错误(`读取日志失败: ${stderr}`);
            }
        } catch (error) {
            toast(`加载日志失败: ${error。message}`);
        }
    }

    // 更新柱状图 (移除裁剪数据)
    function updateBarChart() {
        const storedData = getStoredData();
        const selectedDate = dateSelect。value;

        const filteredData = selectedDate
            ? storedData。filter(entry => entry。date === selectedDate)
            : storedData;

        const aggregatedData = {};
        filteredData。forEach(entry => {
            if (!aggregatedData[entry。date]) {
                aggregatedData[entry。date] = {
                    deletedFiles: 0，
                    deletedDirs: 0，
                    dirtySegments: isExt4 ? undefined : 0，
                    // 移除 trimmedMB
                };
            }
            aggregatedData[entry。date]。deletedFiles += entry。deletedFiles;
            aggregatedData[entry。date]。deletedDirs += entry。deletedDirs;
            if (!isExt4) aggregatedData[entry。date]。dirtySegments += entry。dirtySegments;
            // 移除 trimmedMB 相关代码
        });

        barChart。data。datasets。forEach(dataset => {
            if (dataset。label === '脏段') dataset。hidden = isExt4;
        });
        barChart。update();

        const dates = Object。keys(aggregatedData);
        const deletedFiles = dates。map(date => aggregatedData[date]。deletedFiles);
        const deletedDirs = dates。map(date => aggregatedData[date]。deletedDirs);
        const dirtySegments = dates。map(date => aggregatedData[date]。dirtySegments);
        // 移除 trimmedMB 相关代码

        barChart。data。labels = dates;
        barChart。data。datasets[0]。data = deletedFiles;
        barChart。data。datasets[1]。data = deletedDirs;
        // 确保数据集索引正确
        if (!isExt4) {
            barChart。data。datasets[2]。data = dirtySegments;
        }
        barChart。update();
    }

    // 清除数据
    clearDataBtn。addEventListener('click'， () => {
        clearStoredData();
        updateBarChart();
        toast('数据已清除');
    });

    // 监听日期选择变化
    dateSelect。addEventListener('change'， () => {
        updateBarChart();
    });

    // 加载配置文件
    async function loadConfigFile() {
        try {
            const { errno， stdout， stderr } = await exec('cat /data/media/0/Android/清理规则/配置.txt');
            if (errno === 0) {
                const config = parseConfig(stdout);
                retentionDaysInput。value = config。保留天数 || '30';
                cleanIntervalInput。value = config。程序清理间隔秒数 || '3600';

                const f2fsGcValue = config['f2fs-GC'];
                f2fsGcToggle。checked = f2fsGcValue === 'y';

                if (f2fsGcValue === 'y') {
                    f2fsGcToggleLabel。textContent = '已开启';
                    f2fsGcToggleLabel。classList。remove('btn-outline-secondary');
                    f2fsGcToggleLabel。classList。add('btn-success');
                } else {
                    f2fsGcToggleLabel。textContent = '关闭';
                    f2fsGcToggleLabel。classList。remove('btn-success');
                    f2fsGcToggleLabel。classList。add('btn-outline-secondary');
                }
            } else {
                toast(`错误: ${stderr}`);
            }
        } catch (error) {
            toast(`加载配置失败: ${error。message}`);
        }
    }

    // 解析配置文件
    function parseConfig(text) {
        const config = {};
        const lines = text。split('\n');
        lines。forEach(line => {
            if (line。includes('=')) {
                const [key， value] = line。split('=')。map(item => item。trim());
                if (key && value) {
                    config[key] = value;
                }
            }
        });
        return config;
    }

    // 保存配置文件
    async function saveConfigFile(retentionDays， cleanInterval) {
        try {
            const { errno， stdout， stderr } = await exec('cat /data/media/0/Android/清理规则/配置.txt');
            if (errno !== 0) {
                toast(`读取配置文件失败: ${stderr}`);
                return;
            }

            const lines = stdout。split('\n');
            const updatedLines = lines。map(line => {
                if (line。startsWith('保留天数=')) {
                    return `保留天数=${retentionDays}`;
                } else if (line。startsWith('程序清理间隔秒数=')) {
                    return `程序清理间隔秒数=${cleanInterval}`;
                }
                return line;
            });

            const hasRetentionDays = updatedLines。some(line => line。startsWith('保留天数='));
            const hasCleanInterval = updatedLines。some(line => line。startsWith('程序清理间隔秒数='));

            if (!hasRetentionDays) {
                updatedLines。push(`保留天数=${retentionDays}`);
            }
            if (!hasCleanInterval) {
                updatedLines。push(`程序清理间隔秒数=${cleanInterval}`);
            }

            const updatedConfig = updatedLines。join('\n');
            const { errno: writeErrno， stderr: writeStderr } = await exec(`echo "${updatedConfig}" > /data/media/0/Android/清理规则/配置.txt`);

            if (writeErrno === 0) {
                toast('配置保存成功');
            } else {
                toast(`错误: ${writeStderr}`);
            }
        } catch (error) {
            toast(`保存配置失败: ${error。message}`);
        }
    }

    // 设置 f2fs垃圾回收 参数
    async function setF2fsGc(value) {
        try {
            const configKey = 'f2fs-GC';
            const displayName = isExt4 ? 'EXT4碎片整理' : 'f2fs垃圾回收';

            const { errno， stdout， stderr } = await exec('cat /data/media/0/Android/清理规则/配置.txt');
            if (errno !== 0) throw new 错误(`读取配置失败: ${stderr}`);

            let lines = stdout。split('\n');
            let found = false;
            const updatedLines = lines。map(line => {
                if (line。startsWith(configKey + '=')) {
                    found = true;
                    return `${configKey}=${value}`;
                }
                return line;
            });

            if (!found) updatedLines。push(`${configKey}=${value}`);

            const { errno: writeErr， stderr: writeStderr } = await exec(
                `echo "${updatedLines。join('\n')}" > /data/media/0/Android/清理规则/配置.txt`
            );

            if (writeErr !== 0) throw new 错误(`写入配置失败: ${writeStderr}`);
            toast(`${displayName}已${value === 'y' ? '启用' : '禁用'}`);
        } catch (error) {
            toast(`操作失败: ${error。message}`);
        }
    }

    // 编辑规则文件
    async function editRuleFile(fileName) {
        try {
            const filePath = `/data/media/0/Android/清理规则/${fileName}`;
            const { errno， stderr } = await exec(`am start -a android.intent.action.VIEW -d file://${filePath} -t text/plain`);
            if (errno !== 0) {
                throw new 错误(`编辑文件失败: ${stderr}`);
            }
        } catch (error) {
            toast(`编辑文件失败: ${error。message}`);
        }
    }

    // 刷新日志
    refreshLogBtn。addEventListener('click'， async () => {
        await loadLogFile();
        await updateSegmentInfo();
        toast('数据已刷新');
    });

    // 删除日志
    deleteLogBtn。addEventListener('click'， async () => {
        try {
            const { errno， stderr } = await exec('echo "" > /data/adb/modules/Clean-C/run.log');
            if (errno === 0) {
                await loadLogFile();
                toast('日志已删除');
            } else {
                throw new 错误(`删除日志失败: ${stderr}`);
            }
        } catch (error) {
            toast(`删除日志失败: ${error。message}`);
        }
    });

    // 重启模块
    restartModuleBtn。addEventListener('click'， async () => {
        try {
            const { errno， stderr } = await exec('sh ./rest.sh'， {
                cwd: '/data/adb/modules/Clean-C/'，
            });

            if (stderr) {
                toast(`stderr: ${stderr}`);
            }
            if (errno === 0) {
                toast('模块已重启');
            } else {
                toast(`重启模块失败: ${stderr || '未知错误'}`);
            }
        } catch (error) {
            toast(`模块重启失败: ${error。message}`);
        }
    });

    // 更新段信息
    async function updateSegmentInfo(fsType) {
        try {
            const { errno: getpropErrno， stdout: dataDevice， stderr: getpropStderr } = await exec('getprop dev.mnt.dev.data');
            if (getpropErrno !== 0) {
                throw new 错误(`获取 DATA_DEVICE 失败: ${getpropStderr}`);
            }

            const dataDeviceTrimmed = dataDevice。trim();
            let F2FS_SYSFS = `/sys/fs/f2fs/${dataDeviceTrimmed}`;

            if (fsType && fsType。trim() === 'mifs') {
                F2FS_SYSFS = `/sys/fs/mifs/${dataDeviceTrimmed}`;
            }

            const { errno: dirtySegmentsErrno， stdout: initialDirtySegs， stderr: dirtySegmentsStderr } = await exec(`cat ${F2FS_SYSFS}/dirty_segments`);
            if (dirtySegmentsErrno !== 0) {
                throw new 错误(`获取脏段数量失败: ${dirtySegmentsStderr}`);
            }

            const { errno: freeSegmentsErrno， stdout: currentFreeSegs， stderr: freeSegmentsStderr } = await exec(`cat ${F2FS_SYSFS}/free_segments`);
            if (freeSegmentsErrno !== 0) {
                throw new 错误(`获取空闲段数量失败: ${freeSegmentsStderr}`);
            }

            segmentChart。data。labels = [
                `脏段 (${initialDirtySegs。trim()})`，
                `空闲段 (${currentFreeSegs。trim()})`，
            ];
            segmentChart。data。datasets[0]。data = [initialDirtySegs。trim()， currentFreeSegs。trim()];
            segmentChart。update();

            const { errno: gcUrgentErrno， stdout: gcUrgent， stderr: gcUrgentStderr } = await exec(`cat ${F2FS_SYSFS}/gc_urgent`);
            if (gcUrgentErrno !== 0) {
                throw new 错误(`获取 GC 状态失败: ${gcUrgentStderr}`);
            }

            const value = String(gcUrgent)。trim();
            gcStatusSpan。textContent =
                value === '0' || value。includes('GC_NORMAL') ? 'GC回收关闭' :
                value === '1' || value。includes('GC_URGENT_HIGH') ? 'GC回收开启' :
                '未知';

        } catch (error) {
            toast(`获取段信息失败: ${error。message}`);
        }
    }

    // 绑定按钮事件
    editBlacklist1Btn。addEventListener('click'， () => editRuleFile('blacklist1.txt'));
    editBlacklist2Btn。addEventListener('click'， () => editRuleFile('blacklist2.txt'));
    editWhitelistBtn。addEventListener('click'， () => editRuleFile('whitelist.txt'));

    // 设置 f2fs垃圾回收 开关状态
    f2fsGcToggle。addEventListener('change'， async () => {
        const value = f2fsGcToggle。checked ? 'y' : 'n';
        await setF2fsGc(value);

        if (value === 'y') {
            f2fsGcToggleLabel。textContent = '已开启';
            f2fsGcToggleLabel。classList。remove('btn-outline-secondary');
            f2fsGcToggleLabel。classList。add('btn-success');
        } else {
            f2fsGcToggleLabel。textContent = '关闭';
            f2fsGcToggleLabel。classList。remove('btn-success');
            f2fsGcToggleLabel。classList。add('btn-outline-secondary');
        }
    });

    // 初始化流程
    const fsType = await checkFileSystem();
    await initDatePicker();
    await loadLogFile();
    await loadConfigFile();
    if (fsType !== 'ext4') await updateSegmentInfo(fsType);
    updateBarChart();
});
