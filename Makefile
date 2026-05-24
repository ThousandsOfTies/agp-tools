# AgentCockpit simulation tools

CC_CROSS  = aarch64-linux-gnu-gcc
CC_NATIVE = gcc
LINE ?= 17
DURATION_MS ?= 150
UID ?= 04:AB:CD:EF:01:23
RANGE_MM ?= 300
SCENARIO ?= ../embedded-poc-app/scenarios/sensor_demo_rfid.json
APP_BINARY ?=

SSH_DST = $(if $(KEY),ubuntu@$(EC2),$(EC2))
SSH     = ssh $(if $(KEY),-i $(KEY),)
SCP     = scp $(if $(KEY),-i $(KEY),)

.PHONY: cross native clean deploy-ec2 deploy-app sim-start sim-stop sim-test sim-scenario sim-logs sim-state panel-button panel-rfid panel-rfid-remove panel-range diagnose

cross:
	$(MAKE) -C cuse-stubs cross

native:
	$(MAKE) -C cuse-stubs native

clean:
	$(MAKE) -C cuse-stubs clean

deploy-ec2:
ifndef EC2
	$(error EC2 変数を指定してください: make deploy-ec2 EC2=vibecode-graviton)
endif
	$(MAKE) -C cuse-stubs deploy EC2=$(EC2) KEY=$(KEY)
ifneq ($(APP_BINARY),)
	$(MAKE) deploy-app EC2=$(EC2) KEY=$(KEY) APP_BINARY=$(APP_BINARY)
endif

deploy-app:
ifndef EC2
	$(error EC2 変数を指定してください: make deploy-app EC2=vibecode-graviton APP_BINARY=../embedded-poc-app/app/sensor_demo)
endif
ifndef APP_BINARY
	$(error APP_BINARY 変数を指定してください: make deploy-app EC2=vibecode-graviton APP_BINARY=../embedded-poc-app/app/sensor_demo)
endif
	$(SCP) $(APP_BINARY) $(SSH_DST):~/sensor_demo
	@echo "App deploy complete"

sim-start:
ifndef EC2
	$(error EC2 変数を指定してください: make sim-start EC2=vibecode-graviton)
endif
	$(SSH) $(SSH_DST) 'setsid bash -c "nohup ~/venv/bin/python3 ~/web-bridge/bridge.py > /tmp/bridge.log 2>&1 &" < /dev/null'
	@sleep 2
	$(SSH) $(SSH_DST) 'setsid bash -c "sudo nohup ~/cuse_i2c -f --devname=i2c-1 > /tmp/cuse.log 2>&1 &" < /dev/null'
	@sleep 3
	$(SSH) $(SSH_DST) 'sudo chmod 666 /dev/i2c-1; setsid bash -c "LD_PRELOAD=\"$$HOME/gpio_shim.so $$HOME/spi_shim.so\" nohup ~/sensor_demo > /tmp/sensor.log 2>&1 &" < /dev/null'
	@echo "Simulation started. Open http://$(EC2):8080 or use the forwarded port."

sim-stop:
ifndef EC2
	$(error EC2 変数を指定してください: make sim-stop EC2=vibecode-graviton)
endif
	-$(SSH) $(SSH_DST) 'pkill -f sensor_demo; pkill -f cuse_i2c; pkill -f bridge.py'
	@echo "Simulation processes stopped."

sim-test:
ifndef EC2
	$(error EC2 変数を指定してください: make sim-test EC2=vibecode-graviton)
endif
	$(MAKE) panel-button EC2=$(EC2) KEY=$(KEY) LINE=17 DURATION_MS=150
	@sleep 1
	$(MAKE) panel-rfid EC2=$(EC2) KEY=$(KEY) UID=$(UID)
	@sleep 1
	$(MAKE) sim-state EC2=$(EC2) KEY=$(KEY)
	$(MAKE) sim-logs EC2=$(EC2) KEY=$(KEY)

sim-scenario:
ifndef EC2
	$(error EC2 変数を指定してください: make sim-scenario EC2=vibecode-graviton SCENARIO=scenarios/sensor_demo_rfid.json)
endif
	$(SSH) $(SSH_DST) 'mkdir -p ~/agentcockpit-scenarios'
	$(SCP) scripts/run_scenario.py $(SCENARIO) $(SSH_DST):~/agentcockpit-scenarios/
	$(SSH) $(SSH_DST) 'python3 ~/agentcockpit-scenarios/run_scenario.py ~/agentcockpit-scenarios/$(notdir $(SCENARIO)) --base-url http://127.0.0.1:8080'

sim-logs:
ifndef EC2
	$(error EC2 変数を指定してください: make sim-logs EC2=vibecode-graviton)
endif
	$(SSH) $(SSH_DST) 'echo "--- sensor.log ---"; tail -n 80 /tmp/sensor.log 2>/dev/null; echo "--- bridge.log ---"; tail -n 80 /tmp/bridge.log 2>/dev/null; echo "--- cuse.log ---"; tail -n 80 /tmp/cuse.log 2>/dev/null'

sim-state:
ifndef EC2
	$(error EC2 変数を指定してください: make sim-state EC2=vibecode-graviton)
endif
	$(SSH) $(SSH_DST) 'curl -s http://127.0.0.1:8080/api/state'

panel-button:
ifndef EC2
	$(error EC2 変数を指定してください: make panel-button EC2=vibecode-graviton LINE=17)
endif
	$(SSH) $(SSH_DST) 'curl -s -X POST "http://127.0.0.1:8080/api/button/press?line=$(LINE)&duration_ms=$(DURATION_MS)"'

panel-rfid:
ifndef EC2
	$(error EC2 変数を指定してください: make panel-rfid EC2=vibecode-graviton UID=04:AB:CD:EF:01:23)
endif
	$(SSH) $(SSH_DST) 'curl -s -X POST "http://127.0.0.1:8080/api/rfid/tap?uid=$(UID)"'

panel-rfid-remove:
ifndef EC2
	$(error EC2 変数を指定してください: make panel-rfid-remove EC2=vibecode-graviton)
endif
	$(SSH) $(SSH_DST) 'curl -s -X POST http://127.0.0.1:8080/api/rfid/remove'

panel-range:
ifndef EC2
	$(error EC2 変数を指定してください: make panel-range EC2=vibecode-graviton RANGE_MM=300)
endif
	$(SSH) $(SSH_DST) 'curl -s -X POST "http://127.0.0.1:8080/api/range?value=$(RANGE_MM)"'

diagnose:
ifndef EC2
	$(error EC2 変数を指定してください: make diagnose EC2=vibecode-graviton)
endif
	$(SSH) $(SSH_DST) 'echo "--- processes ---"; pgrep -af "bridge.py|cuse_i2c|sensor_demo" || true; echo "--- devices ---"; ls -l /dev/i2c-1 /dev/gpiochip0 /dev/spidev0.0 2>/dev/null || true; echo "--- api ---"; curl -s http://127.0.0.1:8080/api/state || true'
