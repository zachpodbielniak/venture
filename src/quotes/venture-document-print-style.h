/* Shared by invoice and quote print documents. */
#define VENTURE_DOCUMENT_PRINT_STYLE \
"body{font:14px/1.5 system-ui,sans-serif;color:#111;" \
		"max-width:720px;margin:40px auto;padding:0 20px}" \
		"h1{font-size:22px;margin:0 0 4px}" \
		".head{display:flex;justify-content:space-between;" \
		"align-items:baseline;margin-bottom:28px}" \
		".meta{color:#555;font-size:13px}" \
		"table{width:100%;border-collapse:collapse;margin:20px 0}" \
		"th,td{text-align:left;padding:8px 10px;" \
		"border-bottom:1px solid #ddd}" \
		".num{text-align:right}" \
		"tfoot td{border-bottom:none;font-weight:700}" \
		".status{display:inline-block;padding:2px 10px;" \
		"border:1px solid #999;border-radius:999px;font-size:12px;" \
		"text-transform:uppercase;letter-spacing:0.06em}" \
		".terms{color:#555;font-size:13px;margin-top:24px;" \
		"white-space:pre-wrap}" \
		"@media print{body{margin:0 auto}}"
