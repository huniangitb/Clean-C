// logParser.js

export function parseLogContent(ndjsonContent) {
    if (!ndjsonContent || ndjsonContent.trim() === '') {
        return [];
    }

    const parsedEntries = [];
    const lines = ndjsonContent.split('\n');

    lines.forEach(line => {
        if (line.trim() === '') return;

        try {
            const stats = JSON.parse(line);
            if (!stats.timestamp || !stats.global_stats) return;

            const formattedTimestamp = stats.timestamp.replace('T', ' ').replace('Z', '');
            
            // 从 gc_trim_stats 对象中提取数据，如果不存在则默认为 0
            const reclaimedSegments = (stats.gc_trim_stats && stats.gc_trim_stats.reclaimed_segments) 
                                      ? stats.gc_trim_stats.reclaimed_segments 
                                      : 0;

            const parsedEntry = {
                timestamp: formattedTimestamp,
                date: stats.timestamp.split('T')[0],
                deletedFiles: stats.global_stats.files_deleted || 0,
                deletedDirs: stats.global_stats.dirs_deleted || 0,
                dirtySegments: reclaimedSegments, // 将回收的脏段数赋给 dirtySegments
                trimmedMB: stats.global_stats.megabytes_deleted || 0,
                appStats: stats.app_stats || []
            };
            parsedEntries.push(parsedEntry);

        } catch (error) {
            console.error("解析 JSON 行失败:", error, "行内容:", line);
        }
    });

    return parsedEntries;
}

export function updateLocalStorage(newData) {
    if (!newData || newData.length === 0) return;

    const storedData = JSON.parse(localStorage.getItem('logData') || '[]');
    const dataMap = new Map(storedData.map(entry => [entry.timestamp, entry]));

    newData.forEach(newEntry => {
        dataMap.set(newEntry.timestamp, newEntry);
    });

    const combinedData = Array.from(dataMap.values());
    const cutoffDate = new Date();
    cutoffDate.setDate(cutoffDate.getDate() - 6);
    const filteredData = combinedData.filter(entry => new Date(entry.date) >= cutoffDate);

    localStorage.setItem('logData', JSON.stringify(filteredData));
}

export function getStoredData() {
    return JSON.parse(localStorage.getItem('logData') || '[]');
}

export function clearStoredData() {
    localStorage.removeItem('logData');
}