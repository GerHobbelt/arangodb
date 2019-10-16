#!/bin/bash

set -e

SOURCES=(
    "https://@github.com/arangodb-helper/arangodb.git;arangodb-starter;master;docs/;Manual/"
    "https://@github.com/arangodb/arangosync.git;arangosync;master;docs/;Manual/"
    "https://@github.com/arangodb/kube-arangodb.git;kube-arangodb;master;docs/;Manual/"
    "https://@github.com/arangodb/arangodb-java-driver.git;arangodb-java-driver;master;docs/;Drivers/"
    "https://@github.com/arangodb/arangodb-php.git;arangodb-php;devel;docs/;Drivers/"
    "https://@github.com/arangodb/arangodb-spark-connector.git;arangodb-spark-connector;master;docs/;Drivers/"
    "https://@github.com/arangodb/arangojs.git;arangojs;master;docs/;Drivers/"
    "https://@github.com/arangodb/go-driver.git;go-driver;master;docs/;Drivers/"
    "https://@github.com/arangodb/spring-data.git;spring-data;master;docs/;Drivers/"
)

GITAUTH="$1"

if test "${GITAUTH}" == "git"; then
    ssh-add -l &>/dev/null
    if [ "$?" == 2 ]; then
        # Could not open a connection to your authentication agent.
        eval `ssh-agent`
        ssh-add -l &>/dev/null
        if [ "$?" == 1 ]; then
            # The agent has no identities.
            ssh-add
        fi
    fi
fi

for source in ${SOURCES[@]}; do 

    REPO=$(echo     "$source" |cut -d ';' -f 1)
    CLONEDIR=$(echo "$source" |cut -d ';' -f 2)
    BRANCH=$(echo   "$source" |cut -d ';' -f 3)
    SUBDIR=$(echo   "$source" |cut -d ';' -f 4)
    DST=$(echo      "$source" |cut -d ';' -f 5)

    CODIR="../Books/repos/${CLONEDIR}"
    AUTHREPO="${REPO/@/${GITAUTH}@}"
    if test -d "${CODIR}"; then
        (
            cd "${CODIR}"
            git pull --all
        )
    else
        if test "${GITAUTH}" == "git"; then
            AUTHREPO=$(echo "${AUTHREPO}" | sed -e "s;github.com/;github.com:;" -e "s;https://;;" )
        fi
        git clone "${AUTHREPO}" "${CODIR}"
    fi

    if [ -z "${BRANCH}" ]; then
        echo "No branch specified for ${CLONEDIR}"
        exit 1
    fi

    # checkout branch and pull=merge origin
    (cd "${CODIR}" && git checkout "${BRANCH}" && git pull)

    for oneMD in $(cd "${CODIR}/${SUBDIR}/${DST}"; find "./" -type f |sed "s;\./;;"); do
        NAME=$(basename "${oneMD}")
        MDSUBDIR="${oneMD/${NAME}/}"
        DSTDIR="../Books/${DST}/${MDSUBDIR}"
        TOPREF=$(echo "${MDSUBDIR}" | sed 's;\([a-zA-Z]*\)/;../;g')
        if test ! -d "${DSTDIR}"; then
            mkdir -p "${DSTDIR}"
        fi
        sourcefile="${CODIR}/${SUBDIR}/${DST}/${oneMD}"
        targetfile="${DSTDIR}/${NAME}"
        if [[ "$sourcefile" == *.md ]]; then
            sed "s;https://docs.arangodb.com/latest;../${TOPREF};g" "$sourcefile" > "$targetfile"
        else
            cp "$sourcefile" "$targetfile"
        fi
    done
done
