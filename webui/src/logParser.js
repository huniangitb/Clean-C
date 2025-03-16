/**
 * 解析日志内容
 * @param {string} logContent - 日志文件内容
 * @returns {Array} - 返回解析后的数据数组
 */
export function parseLogContent(logContent) {
    const lines = logContent.split('\n');
    const dataByTimestamp = {}; // 按时间戳存储数据

    lines.forEach(line => {
        // 提取时间戳（精确到秒）
        const timestampMatch = line.match(/^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}/);
        if (!timestampMatch) return; // 如果时间戳不存在，跳过该行

        const timestamp = timestampMatch[0];

        // 初始化当前时间戳的数据
        if (!dataByTimestamp[timestamp]) {
            dataByTimestamp[timestamp] = {
                timestamp: timestamp, // 时间戳
                date: timestamp.split(' ')[0], // 提取日期部分
                deletedFiles: 0,
                deletedDirs: 0,
                dirtySegments: 0,
                
            };
        }

        // 解析日志内容
        if (line.includes('已删除文件数:')) {
            const match = line.match(/已删除文件数:\s*(\d+)/);
            if (match) {
                dataByTimestamp[timestamp].deletedFiles += parseInt(match[1], 10);
            }
        } else if (line.includes('已删除目录数:')) {
            const match = line.match(/已删除目录数:\s*(\d+)/);
            if (match) {
                dataByTimestamp[timestamp].deletedDirs += parseInt(match[1], 10);
            }
        } else if (line.includes('已回收') && line.includes('个脏段')) {
            const match = line.match(/已回收\s*(\d+)\s*个脏段/);
            if (match) {
                dataByTimestamp[timestamp].dirtySegments += parseInt(match[1], 10);
            }
        } // 移除 裁剪数据解析
        /* else if (line.includes('已裁剪') && line.includes('MB')) {
            const match = line.match(/已裁剪\s*([\d.]+)\s*MB/);
       if (match) {
           dataByTimestamp[timestamp].trimmedMB += parseFloat(match[1]);
       }
        } */
    });

    // 将按时间戳存储的数据转换为数组
    return Object.values(dataByTimestamp);
}
/**
 * 更新 localStorage 中的数据
 * @param {Array} newData - 新解析的日志数据
 */
export function updateLocalStorage(newData) {
    const storedData = JSON.parse(localStorage.getItem('logData') || '[]');

    // 将新数据按时间戳合并到存储的数据中
    newData.forEach(newEntry => {
        const existingEntry = storedData.find(entry => entry.timestamp === newEntry.timestamp);
        if (existingEntry) {
            // 如果时间戳已存在，更新数值（避免重复累加）
            existingEntry.deletedFiles = newEntry.deletedFiles;
            existingEntry.deletedDirs = newEntry.deletedDirs;
            existingEntry.dirtySegments = newEntry.dirtySegments;
            // 移除 existingEntry.trimmedMB = newEntry.trimmedMB;
        } else {
            // 否则添加新数据
            storedData.push(newEntry);
        }
    });

    // 只保留最近7天的数据
    const cutoffDate = new Date();
    cutoffDate.setDate(cutoffDate.getDate() - 6); // 保留7天内的数据
    const filteredData = storedData.filter(entry => new Date(entry.date) >= cutoffDate);

    localStorage.setItem('logData', JSON.stringify(filteredData));
}

/**
 * 获取 localStorage 中存储的数据
 * @returns {Array} - 返回存储的日志数据
 */
export function getStoredData() {
    return JSON.parse(localStorage.getItem('logData') || '[]');
}

/**
 * 清除 localStorage 中的数据
 */
export function clearStoredData() {
    localStorage.removeItem('logData');
}
