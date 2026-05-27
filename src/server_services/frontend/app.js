const map = L.map('map').setView([41.9028, 12.4964], 13);

L.tileLayer('https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png', {
    attribution: '&copy; OpenStreetMap contributors'
}).addTo(map);

async function loadPaths() {
    const response = await fetch('http://docker:8001/paths');
    const data = await response.json();

    const bounds = [];

    data.forEach(path => {
       const start = [
    	path.from.latitude,
    	path.from.longitude
	];

	const end = [
    	path.to.latitude,
    	path.to.longitude
	];        
	bounds.push(start);
        bounds.push(end);

        const polyline = L.polyline(
            [start, end],
            {
                weight: 5
            }
        ).addTo(map);

	polyline.bindPopup(`
		<b>Score:</b> ${path.score}<br>
		<b>Start:</b> ${path.from.timestamp}<br>
		<b>End:</b> ${path.to.timestamp}
        `);

        L.marker(start)
            .addTo(map)
            .bindPopup('Start');

        L.marker(end)
            .addTo(map)
            .bindPopup('End');
    });

    if (bounds.length > 0) {
        map.fitBounds(bounds);
    }
}

loadPaths();
