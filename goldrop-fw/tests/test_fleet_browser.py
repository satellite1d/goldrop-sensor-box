"""Exercise fleet forms against real local HTTP/SQLite with synthetic enrolled boards."""

import json
from pathlib import Path
import time
from playwright.sync_api import sync_playwright, expect
from test_fleet_management import FleetTests, MAC, OTHER, running, parse_message, wire


def main():
    fixture = FleetTests()
    fixture.setUp()
    try:
        fixture.release()
        with running(fixture.store) as (_, web), sync_playwright() as playwright:
            web.management = fixture.fleet
            browser = playwright.chromium.launch(headless=True)
            page = browser.new_page(viewport={"width": 1440, "height": 1100}, locale="pl-PL")
            errors = []
            page.on("pageerror", lambda error: errors.append(str(error)))
            page.goto(f"http://127.0.0.1:{web.server_port}/")
            page.locator("#tab-management").click()
            expect(page.locator("#fleet-devices tr")).to_have_count(2)
            expect(page.locator("#fleet-interval")).to_be_disabled()
            page.locator(f'input[data-mac="{MAC}"]').focus()
            page.evaluate(
                "window.savedReleaseOption = document.querySelector('#fleet-release option')"
            )
            page.evaluate("document.querySelector('#fleet-refresh').click()")
            expect(page.locator("#fleet-devices tr")).to_have_count(2)
            # Waiting for the response and render avoids asserting on the old DOM.
            page.wait_for_timeout(300)
            expect(page.locator(f'input[data-mac="{MAC}"]')).to_be_focused()
            assert page.evaluate(
                "savedReleaseOption === document.querySelector('#fleet-release option')"
            )
            page.locator("#fleet-all").click()
            page.locator("#fleet-interval").select_option("60")
            page.locator("#fleet-sampling button").click()
            expect(page.locator("#fleet-result")).to_contain_text("interwał pomiarów")
            expect(page.locator("#fleet-devices")).to_contain_text("W kolejce: interwał pomiarów")
            for mac in (MAC, OTHER):
                assert fixture.exchange(mac)[4] == 7
                fixture.exchange(mac, seq=2, high=1, result=1, result_rev=1)
                fixture.store.save(
                    parse_message(
                        wire(
                            dict(
                                schema_version=1,
                                message_type="status",
                                device_id=mac,
                                firmware_version="0.6.0",
                                hardware_revision="goldrop-c3-v1.0",
                                sampling_interval_ms=60000,
                            )
                        )
                    ),
                    int(time.time() * 1000),
                )
            page.locator("#fleet-refresh").click()
            expect(page.locator("#fleet-devices")).to_contain_text("Pomiar co 60 s")
            expect(page.locator("#fleet-devices")).not_to_contain_text("W kolejce")
            page.locator("#fleet-none").click()
            page.locator(f'input[data-mac="{MAC}"]').check()
            page.locator("#fleet-ssid").fill("future-network")
            page.locator("#fleet-password").fill("synthetic-secret")
            page.locator('#fleet-wifi button[type="submit"]').click()
            expect(page.locator("#fleet-result")).to_contain_text("przygotowanie Wi-Fi")
            expect(page.locator("#fleet-password")).to_have_value("")
            command = fixture.exchange(MAC, seq=2, high=1, result=1, result_rev=1)
            assert command[4] == 2
            operation = command[32:48]
            fixture.exchange(
                MAC, seq=3, high=2, pending=2, phase=1, result=2, result_rev=2, operation=operation
            )
            page.locator("#fleet-refresh").click()
            expect(page.locator("#fleet-devices")).to_contain_text("Przygotowane")
            page.locator("#fleet-arm").click()
            expect(page.locator("#fleet-result")).to_contain_text("uzbrojenie")
            assert (
                fixture.exchange(
                    MAC,
                    seq=3,
                    high=2,
                    pending=2,
                    phase=1,
                    result=2,
                    result_rev=2,
                    operation=operation,
                )[4]
                == 4
            )
            arm = fixture.registry.status(MAC)[0]["queued_revision"]
            # End this synthetic trial before testing a new independent OTA command.
            fixture.exchange(MAC, seq=4, high=arm, result=4, result_rev=arm)
            page.locator("#fleet-refresh").click()
            page.locator("#fleet-ota-confirm").check()
            page.locator('#fleet-ota button[type="submit"]').click()
            expect(page.locator("#fleet-result")).to_contain_text("sprawdzenie OTA")
            assert fixture.registry.ota_target(MAC) == "manifest-0.6.1.json"
            assert fixture.registry.ota_target(OTHER) is None
            public = page.request.get(f"http://127.0.0.1:{web.server_port}/api/management").text()
            assert "synthetic-secret" not in public
            assert "synthetic-secret" not in page.locator("body").inner_text()
            page.set_viewport_size({"width": 390, "height": 844})
            assert page.evaluate("document.documentElement.scrollWidth <= innerWidth + 1")
            page.screenshot(
                path=str(Path(__file__).resolve().parents[1] / "build" / "fleet-mobile.png"),
                full_page=True,
            )
            page.locator("#tab-measurements").click()
            expect(page.locator("#measurement-panel")).to_be_visible()
            web.management = None
            page.locator("#tab-management").click()
            expect(page.locator("#fleet-disabled")).to_be_visible()
            expect(page.locator("#fleet-interval")).to_be_disabled()
            assert not errors, errors
            browser.close()
    finally:
        fixture.tearDown()
    print(
        "Fleet browser: selection, sampling, Wi-Fi stage/arm, selected OTA, mobile and disabled mode passed"
    )


if __name__ == "__main__":
    main()
