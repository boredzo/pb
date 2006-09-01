Deployment: main.c
	xcodebuild -configuration Deployment
Development: main.c
	xcodebuild -configuration Development

.PHONY: Deployment Development
