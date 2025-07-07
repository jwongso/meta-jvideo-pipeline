#!/usr/bin/env python3
"""Service Controller - Control plane for Juni's video pipeline services"""

import json
import subprocess
import sys
import time
import os
import logging
from pathlib import Path
from datetime import datetime

try:
    import redis
    REDIS_AVAILABLE = True
except ImportError:
    REDIS_AVAILABLE = False

class ServiceController:
    """Controls video pipeline services via systemd"""

    def __init__(self):
        self.setup_logging()
        self.services = {
            'frame-publisher': {
                'unit_template': 'jvideo-frame-publisher-{lang}.service',
                'languages': ['python', 'cpp', 'rust'],
                'default': 'cpp'
            },
            'frame-resizer': {
                'unit_template': 'jvideo-frame-resizer-{lang}.service',
                'languages': ['python', 'cpp'],
                'default': 'cpp'
            },
            'frame-saver': {
                'unit_template': 'jvideo-frame-saver-{lang}.service',
                'languages': ['python', 'cpp'],
                'default': 'cpp'
            },
            'queue-monitor': {
                'unit_template': 'jvideo-queue-monitor-{lang}.service',
                'languages': ['python', 'cpp'],
                'default': 'cpp'
            }
        }

        # Setup Redis if available
        self.redis = None
        if REDIS_AVAILABLE:
            try:
                self.redis = redis.Redis(host='localhost', decode_responses=True)
                self.redis.ping()
                self.logger.info("Redis connection established")
            except Exception as e:
                self.logger.warning(f"Redis not available: {e}")
                self.redis = None

    def setup_logging(self):
        """Setup logging"""
        self.logger = logging.getLogger('service-controller')
        self.logger.setLevel(logging.INFO)

        # Console handler
        handler = logging.StreamHandler()
        formatter = logging.Formatter('%(asctime)s - %(name)s - %(levelname)s - %(message)s')
        handler.setFormatter(formatter)
        self.logger.addHandler(handler)

        # File handler
        os.makedirs('/var/log/jvideo', exist_ok=True)
        fh = logging.FileHandler('/var/log/jvideo/service-controller.log')
        fh.setFormatter(formatter)
        self.logger.addHandler(fh)

    def _run_systemctl(self, *args):
        """Run systemctl command"""
        cmd = ['systemctl'] + list(args)
        result = subprocess.run(cmd, capture_output=True, text=True)
        return result.returncode == 0, result.stdout, result.stderr

    def get_active_implementation(self, service):
        """Get currently active implementation of a service"""
        service_info = self.services.get(service)
        if not service_info:
            return None

        # Handle single implementation services
        if service_info.get('single_impl'):
            unit = service_info['unit_template']
            success, stdout, _ = self._run_systemctl('is-active', unit)
            if success and stdout.strip() == 'active':
                return service_info['default']
            return None

        # Check each language implementation
        for lang in service_info['languages']:
            unit = service_info['unit_template'].format(lang=lang)
            success, stdout, _ = self._run_systemctl('is-active', unit)
            if success and stdout.strip() == 'active':
                return lang

        return None

    def switch_implementation(self, service, new_lang):
        """Switch a service to different language implementation"""
        service_info = self.services.get(service)
        if not service_info:
            self.logger.error(f"Unknown service: {service}")
            return False

        # Don't try to switch single-implementation services
        if service_info.get('single_impl', False):
            self.logger.info(f"{service} has only one implementation")
            unit = service_info['unit_template']
            self.logger.info(f"Starting {unit}")
            success, _, stderr = self._run_systemctl('start', unit)
            if success:
                self.logger.info(f"Successfully started {service}")
                return True
            else:
                self.logger.error(f"Failed to start {service}: {stderr}")
                return False

        # Normal implementation switching logic
        if new_lang not in service_info['languages']:
            self.logger.error(f"Language {new_lang} not available for {service}")
            return False

        # Check if the implementation exists
        new_unit = service_info['unit_template'].format(lang=new_lang)
        success, _, _ = self._run_systemctl('list-unit-files', new_unit)
        if not success:
            self.logger.warning(f"Service unit {new_unit} not found, trying default")
            return self.switch_implementation(service, service_info['default'])

        # Stop all implementations of this service
        current = self.get_active_implementation(service)
        if current:
            if service_info.get('single_impl'):
                current_unit = service_info['unit_template']
            else:
                current_unit = service_info['unit_template'].format(lang=current)

            self.logger.info(f"Stopping {current_unit}")
            self._run_systemctl('stop', current_unit)
            time.sleep(2)  # Give service time to stop

        # Start new implementation
        self.logger.info(f"Starting {new_unit}")
        success, _, stderr = self._run_systemctl('start', new_unit)

        if success:
            self.logger.info(f"Successfully switched {service} to {new_lang}")

            # Update Redis if available
            if self.redis:
                try:
                    self.redis.hset(f'service:{service}', mapping={
                        'language': new_lang,
                        'unit': new_unit,
                        'switched_at': datetime.now().isoformat()
                    })
                except:
                    pass

            return True
        else:
            self.logger.error(f"Failed to start {new_unit}: {stderr}")
            # Try to start default implementation
            if new_lang != service_info['default']:
                self.logger.info(f"Falling back to default implementation")
                return self.switch_implementation(service, service_info['default'])
            return False

    def start_service(self, service, lang=None):
        """Start a service with specified or default language"""
        service_info = self.services.get(service)
        if not service_info:
            self.logger.error(f"Unknown service: {service}")
            return False

        # Handle single-implementation services directly
        if service_info.get('single_impl', False):
            unit = service_info['unit_template']
            self.logger.info(f"Starting {unit}")
            success, _, stderr = self._run_systemctl('start', unit)
            if success:
                self.logger.info(f"Successfully started {service}")
                return True
            else:
                self.logger.error(f"Failed to start {service}: {stderr}")
                return False

        # For multi-implementation services, use switch_implementation
        if lang is None:
            lang = service_info['default']
        return self.switch_implementation(service, lang)

    def stop_service(self, service):
        """Stop a service"""
        current = self.get_active_implementation(service)
        if not current:
            self.logger.info(f"{service} is not running")
            return True

        service_info = self.services.get(service)
        if service_info.get('single_impl'):
            unit = service_info['unit_template']
        else:
            unit = service_info['unit_template'].format(lang=current)

        self.logger.info(f"Stopping {unit}")
        success, _, stderr = self._run_systemctl('stop', unit)

        if success:
            self.logger.info(f"Successfully stopped {service}")
            if self.redis:
                try:
                    self.redis.hset(f'service:{service}', 'status', 'stopped')
                except:
                    pass
            return True
        else:
            self.logger.error(f"Failed to stop {unit}: {stderr}")
            return False

    def get_status(self):
        """Get status of all services"""
        status = {}

        for service, info in self.services.items():
            active_lang = self.get_active_implementation(service)
            status[service] = {
                'active': active_lang is not None,
                'language': active_lang or 'none',
                'available_languages': info['languages']
            }

            # Get detailed systemd status
            if active_lang:
                if info.get('single_impl'):
                    unit = info['unit_template']
                else:
                    unit = info['unit_template'].format(lang=active_lang)

                # Get service details
                success, stdout, _ = self._run_systemctl('show', unit,
                    '--property=MainPID,ActiveState,SubState,ExecMainStartTimestamp')

                if success:
                    for line in stdout.strip().split('\n'):
                        if '=' in line:
                            key, value = line.split('=', 1)
                            if key == 'MainPID':
                                status[service]['pid'] = value
                            elif key == 'ActiveState':
                                status[service]['state'] = value
                            elif key == 'SubState':
                                status[service]['substate'] = value
                            elif key == 'ExecMainStartTimestamp' and value:
                                status[service]['started_at'] = value

        return status

    def apply_configuration(self, config_file='/etc/jvideo/services.conf'):
        """Apply configuration from file"""
        if not os.path.exists(config_file):
            self.logger.error(f"Configuration file not found: {config_file}")
            return False

        try:
            with open(config_file, 'r') as f:
                config = json.load(f)

            self.logger.info(f"Applying configuration from {config_file}")

            for service, service_config in config.get('services', {}).items():
                if service not in self.services:
                    self.logger.warning(f"Unknown service in config: {service}")
                    continue

                if service_config.get('enabled', False):
                    lang = service_config.get('language', self.services[service]['default'])
                    current = self.get_active_implementation(service)

                    if current != lang:
                        self.logger.info(f"Switching {service} from {current} to {lang}")
                        self.switch_implementation(service, lang)
                    elif not current:
                        self.logger.info(f"Starting {service} with {lang}")
                        self.start_service(service, lang)
                else:
                    # Stop service if disabled
                    if self.get_active_implementation(service):
                        self.logger.info(f"Stopping disabled service: {service}")
                        self.stop_service(service)

            return True

        except json.JSONDecodeError as e:
            self.logger.error(f"Invalid JSON in configuration file: {e}")
            return False
        except Exception as e:
            self.logger.error(f"Error applying configuration: {e}")
            return False

    def monitor_services(self):
        """Monitor services and update Redis"""
        while True:
            try:
                status = self.get_status()

                # Update Redis with current status
                if self.redis:
                    try:
                        self.redis.set('controller:status', json.dumps(status), ex=30)
                        self.redis.set('controller:last_update', datetime.now().isoformat())
                    except:
                        pass

                # Log status
                active_services = [s for s, info in status.items() if info['active']]
                self.logger.debug(f"Active services: {', '.join(active_services)}")

                time.sleep(10)

            except KeyboardInterrupt:
                break
            except Exception as e:
                self.logger.error(f"Error in monitor loop: {e}")
                time.sleep(30)

    def print_status_table(self, status):
        """Print a nice status table"""
        print("\nJuni's Video Pipeline - Service Status")
        print("=" * 70)
        print(f"{'Service':<20} {'Status':<10} {'Language':<10} {'PID':<8} {'State':<15}")
        print("-" * 70)

        for service, info in status.items():
            status_str = "Active" if info['active'] else "Stopped"
            lang = info.get('language', 'none')
            pid = info.get('pid', '-')
            state = info.get('substate', '-')

            print(f"{service:<20} {status_str:<10} {lang:<10} {pid:<8} {state:<15}")

        print("-" * 70)
        print(f"Controller time: {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")


def main():
    controller = ServiceController()

    if len(sys.argv) < 2:
        print("Juni's Video Pipeline - Service Controller")
        print()
        print("Usage:")
        print("  control status                    - Show service status")
        print("  control start <service> [lang]    - Start service")
        print("  control stop <service>            - Stop service")
        print("  control switch <service> <lang>   - Switch implementation")
        print("  control restart <service>         - Restart service")
        print("  control apply [config-file]       - Apply configuration")
        print("  control monitor                   - Monitor services (continuous)")
        print()
        print("Services: frame-publisher, frame-resizer, frame-saver, queue-monitor")
        print("Languages: python, cpp, rust")
        return

    command = sys.argv[1]

    try:
        if command == 'status':
            status = controller.get_status()
            controller.print_status_table(status)

            # Also output JSON if redirected
            if not sys.stdout.isatty():
                print()
                print(json.dumps(status, indent=2))

        elif command == 'start':
            if len(sys.argv) < 3:
                print("Error: Service name required")
                return

            service = sys.argv[2]
            lang = sys.argv[3] if len(sys.argv) > 3 else None

            if controller.start_service(service, lang):
                print(f"Successfully started {service}")
            else:
                print(f"Failed to start {service}")
                sys.exit(1)

        elif command == 'stop':
            if len(sys.argv) < 3:
                print("Error: Service name required")
                return

            service = sys.argv[2]
            if controller.stop_service(service):
                print(f"Successfully stopped {service}")
            else:
                print(f"Failed to stop {service}")
                sys.exit(1)

        elif command == 'switch':
            if len(sys.argv) < 4:
                print("Error: Service name and language required")
                return

            service = sys.argv[2]
            lang = sys.argv[3]

            if controller.switch_implementation(service, lang):
                print(f"Successfully switched {service} to {lang}")
            else:
                print(f"Failed to switch {service} to {lang}")
                sys.exit(1)

        elif command == 'restart':
            if len(sys.argv) < 3:
                print("Error: Service name required")
                return

            service = sys.argv[2]
            current = controller.get_active_implementation(service)

            if current:
                controller.stop_service(service)
                time.sleep(2)
                if controller.start_service(service, current):
                    print(f"Successfully restarted {service}")
                else:
                    print(f"Failed to restart {service}")
                    sys.exit(1)
            else:
                print(f"{service} is not running")

        elif command == 'apply':
            config_file = sys.argv[2] if len(sys.argv) > 2 else '/etc/jvideo/services.conf'

            if controller.apply_configuration(config_file):
                print("Configuration applied successfully")

                # Show new status
                status = controller.get_status()
                controller.print_status_table(status)
            else:
                print("Failed to apply configuration")
                sys.exit(1)

        elif command == 'monitor':
            print("Starting service monitor (Ctrl+C to stop)...")
            controller.monitor_services()

        else:
            print(f"Unknown command: {command}")
            sys.exit(1)

    except Exception as e:
        print(f"Error: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
