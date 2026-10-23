# Convenience targets. Everything is plain CMake / docker compose underneath.
BUILD ?= build

.PHONY: all build test e2e e2e-cockpit measure-isolation keys demo-local demo-cockpit up up-cockpit down publish-1.1 publish-1.2 logs clean

all: build

build:
	cmake -S . -B $(BUILD) -DCMAKE_BUILD_TYPE=Release
	cmake --build $(BUILD) -j

test: build
	ctest --test-dir $(BUILD) --output-on-failure

e2e: build
	QT_QPA_PLATFORM=offscreen tests/e2e_ota.sh $(BUILD)

e2e-cockpit: build
	QT_QPA_PLATFORM=offscreen tests/e2e_cockpit.sh $(BUILD)

measure-isolation: build
	tools/measure_isolation.sh $(BUILD)

keys:
	tools/gen_keys.sh

demo-local: build
	tools/run_local_demo.sh

demo-cockpit: build
	tools/run_cockpit_demo.sh

# ---- containers ----
up: keys
	docker compose up --build -d
	@echo "Cluster: open a VNC viewer on localhost:5900"

up-cockpit: keys
	docker compose --profile cockpit up --build -d
	@echo "Cockpit: VNC localhost:5901 | faults: docker compose exec cockpit sdv-hypervisor --inject ivi:hang"

down:
	docker compose --profile cockpit down -v

publish-1.1:
	docker compose run --rm publisher 1.1.0 "New Horizon theme and live efficiency widget"

publish-1.2:
	docker compose run --rm publisher 1.2.0 "Navigation preview (will fail health check)"

logs:
	docker compose logs -f ota-agent hmi

clean:
	rm -rf $(BUILD) .demo
