from datetime import date, timedelta
start = date(2024, 2, 28)
for offset in (1441, -1441, 2880, -2880, 2147483647, -2147483648):
    for hhmm in (0, 900, 2359, 2400):
        minute = (hhmm // 100) * 60 + hhmm % 100 + offset
        days, rem = divmod(minute, 1440)
        try:
            result_date = (start + timedelta(days=days)).isoformat()
        except OverflowError:
            result_date = 'OUTSIDE_PYTHON_DATE_RANGE'
        left = rem // 60 * 100 + rem % 60
        right = 2400 if rem == 0 else left
        print(f'offset={offset} time={hhmm:04} days={days} date={result_date} left={left} right={right}')
