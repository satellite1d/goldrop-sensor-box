"""Optional real-browser check. Uses synthetic data, never a real device or database."""

import math
from pathlib import Path
import tempfile
import time

from playwright.sync_api import sync_playwright, expect
from test_measurement_backend import MeasurementStore, parse_message, running, sample, wire, MAC


def main():
    output = Path(__file__).resolve().parents[1] / "build"
    other = "02:00:00:00:00:02"
    now = int(time.time() * 1000)
    with tempfile.TemporaryDirectory() as root:
        store = MeasurementStore(Path(root) / "private" / "measurements.sqlite")
        for i in range(350):
            stamp = now - (349 - i) * 10000
            message = sample(
                sequence=str(i + 1),
                timestamp_ms=None if i == 348 else stamp,
                temperature_sht_c=23 + 0.7 * math.sin(i / 35),
                humidity_rh=48 + 5 * math.sin(i / 45),
                temperature_ds18_c=None if i == 347 else 21 + 0.4 * math.sin(i / 40),
                ds18b20_valid=i != 347,
                ds18b20_error="disconnected" if i == 347 else "none",
            )
            store.save(parse_message(wire(message)), stamp)
        for mac in (MAC, other):
            store.save(
                parse_message(
                    wire(
                        dict(
                            schema_version=1,
                            message_type="status",
                            device_id=mac,
                            firmware_version="0.5.0",
                            buffered=0,
                            utc_synchronized=True,
                            delivery_confirmation="backend_ack",
                        )
                    )
                ),
                now,
            )
        store.name(MAC, "Chłodnia 1 · dane testowe")
        store.name(other, "Magazyn · dane testowe")
        with running(store) as (_, web), sync_playwright() as playwright:
            browser = playwright.chromium.launch(headless=True)
            context = browser.new_context(
                viewport={"width": 1440, "height": 1100},
                timezone_id="Europe/Warsaw",
                locale="pl-PL",
                accept_downloads=True,
            )
            page = context.new_page()
            # Relative chart filters must tolerate a wrong browser clock.
            page.add_init_script("const originalNow = Date.now; Date.now = () => originalNow() + 5 * 86400000;")
            failures = []
            page.on("pageerror", lambda error: failures.append(str(error)))
            page.goto(f"http://127.0.0.1:{web.server_port}/")
            expect(page.locator("#device-count")).to_have_text("2")
            page.locator(f'[data-device="{MAC}"]').click()
            expect(page.locator("#history tr")).to_have_count(200)
            page.locator("#rename").click()
            page.locator("#name-input").fill("Nowa nazwa płytki")
            page.locator("#name-save").click()
            expect(page.locator("#device-name")).to_have_text("Nowa nazwa płytki")
            expect(page.locator(f'[data-device="{MAC}"]')).to_contain_text("Nowa nazwa płytki")
            page.reload()
            page.locator(f'[data-device="{MAC}"]').click()
            expect(page.locator("#device-name")).to_have_text("Nowa nazwa płytki")
            page.locator("#rename").click()
            page.locator("#name-input").fill("Anulowana nazwa")
            page.locator("#name-cancel").click()
            expect(page.locator("#device-name")).to_have_text("Nowa nazwa płytki")
            page.locator("#rename").click()
            page.locator("#name-input").fill("")
            page.locator("#name-save").click()
            expect(page.locator("#device-name")).to_have_text("Urządzenie " + MAC[-5:])
            expect(page.locator("#temperature-chart .series")).to_have_count(2)
            expect(page.locator("#humidity-chart .series")).to_have_count(1)
            expect(page.locator("#history")).to_contain_text("Brak czasu UTC")
            expect(page.locator("#history")).to_contain_text("niepodłączony")
            expect(page.locator("#timezone")).to_have_text("Europe/Warsaw")
            expect(page.locator("#ds")).not_to_have_text("—")
            with page.expect_download() as transfer:
                page.locator("#export").click()
            csv_data = Path(transfer.value.path()).read_text(encoding="utf-8-sig")
            assert len(csv_data.splitlines()) == 201
            assert "boot_id;sequence;timestamp_ms" in csv_data
            page.locator("#next").click()
            expect(page.locator("#history tr")).to_have_count(150)
            expect(page.locator("#history-note")).to_contain_text("wstrzymane")
            with page.expect_download() as all_transfer:
                page.locator("#export-range").click()
            all_csv = Path(all_transfer.value.path()).read_text(encoding="utf-8-sig")
            assert len(all_csv.splitlines()) == 351
            assert "before_id" not in page.locator("#export-range").get_attribute("href")
            page.locator("#previous").click()
            expect(page.locator("#history tr")).to_have_count(200)
            before_path = page.locator("#temperature-chart .series").first.get_attribute("d")
            store.save(parse_message(wire(sample(sequence="351", timestamp_ms=int(time.time() * 1000), temperature_sht_c=29))), int(time.time() * 1000))
            expect(page.locator("#sht")).to_have_text("29,00", timeout=15000)
            expect(page.locator("#temperature-chart .series").first).not_to_have_attribute("d", before_path)
            page.locator("#period").select_option("custom")
            page.locator("#from").fill("2026-01-01T12:00")
            page.locator("#to").fill("2026-02-01T12:00")
            page.locator("#apply-range").click()
            expect(page.locator("#error")).to_contain_text("maksymalnie 7 dni")
            page.locator("#period").select_option("1")
            expect(page.locator("#error")).to_be_hidden()
            page.locator(f'[data-device="{other}"]').click()
            expect(page.locator("#no-history")).to_be_visible()
            expect(page.locator("#history tr")).to_have_count(0)
            store.name(other, '<img src=x onerror="alert(1)">')
            page.locator("#refresh").click()
            expect(page.locator("#device-name")).to_have_text('<img src=x onerror="alert(1)">')
            assert page.locator("img").count() == 0
            store.name(other, "Magazyn · dane testowe")
            page.locator(f'[data-device="{MAC}"]').click()
            expect(page.locator("#history tr")).to_have_count(200)
            page.screenshot(path=output / "measurements-desktop.png", full_page=True)
            page.set_viewport_size({"width": 390, "height": 844})
            expect(page.locator("#temperature-chart")).to_have_attribute("viewBox", "0 0 400 240")
            assert page.evaluate("document.documentElement.scrollWidth <= innerWidth")
            page.screenshot(path=output / "measurements-mobile.png", full_page=True)
            page.locator(f'[data-device="{other}"]').click()
            expect(page.locator("#no-history")).to_be_visible()
            page.route("**/api/devices", lambda route: route.abort())
            page.locator("#refresh").click()
            expect(page.locator("#error")).to_be_visible()
            page.unroute("**/api/devices")
            page.locator("#refresh").click()
            expect(page.locator("#error")).to_be_hidden()
            store.save(parse_message(wire(sample(sequence="352", timestamp_ms=now - 5 * 86400000, temperature_sht_c=31))), int(time.time() * 1000))
            page.locator(f'[data-device="{MAC}"]').click()
            expect(page.locator("#sht")).to_have_text("31,00")
            expect(page.locator("#last-measurement")).to_contain_text("5 dni temu")
            expect(page.locator("#data-time")).to_contain_text("Odebrano:")
            expect(page.locator("#data-time")).to_contain_text("możliwe zaległości")
            expect(page.locator("#chart-note")).to_contain_text("poza wybranym zakresem")
            assert not failures, failures
            browser.close()
    print(
        "PASS: desktop/mobile, charts, pagination, CSV, local time, missing sensor, XSS, reconnect"
    )


if __name__ == "__main__":
    main()
