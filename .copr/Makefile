srpm:
	dnf -y install git make gcc flex bison bzip2 dwarves xz rpm-build python3-pyyaml rust rust-src bindgen rustfmt
	git config user.email "fedora@leemhuis.info"
	git config user.name "Thorsten Leemhuis"
	.copr/deepen_gittree.sh

	# set UPSTREAMBUILD_GIT_ONLY=0 to include date in %release when building snapshots
	make NO_CONFIGCHECKS=1 DIST=.fc41 UPSTREAMBUILD_GIT_ONLY=0 dist-srpm

	mv redhat/rpm/SRPMS/*.src.rpm  $(outdir)
