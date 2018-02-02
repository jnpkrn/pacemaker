<!--
 Copyright 2018 Red Hat, Inc.
 Author: Jan Pokorny <jpokorny@redhat.com>
 Part of pacemaker project
 SPDX-License-Identifier: GPL-2.0-or-later
 -->
<xsl:stylesheet version="1.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:cibtr="http://clusterlabs.org/ns/pacemaker/cibtr-2">
<xsl:output method="xml" encoding="UTF-8" indent="yes" omit-xml-declaration="yes"/>

<xsl:param name="cib-min-ver" select="'3.0'"/>

<!--
 helper definitions
 -->

<cibtr:map>

  <!--
   Target tag:     primitive
                   template
   Object:         ./operations/op/@*
                   ./operations/op/meta_attributes/nvpair/@name
   Selector ctxt:  ./operations/op/@name
   Move ctxt:      meta_attributes -> ./meta_attributes/nvpair
   -->
  <cibtr:table for="resources-operation" msg-prefix="Resources-operation">
    <cibtr:replace what="requires"
                   with=""
                   msg-extra="only start/promote operation taken into account"/>
    <cibtr:replace what="requires"
                   with="requires"
                   in-case-of="start|promote"
                   where="meta_attributes"/>
  </cibtr:table>

  <!--
   Target tag:     rsc_colocation
   Object:         ./@*
   Selector ctxt:  N/A
   Move ctxt:      N/A
   -->
  <cibtr:table for="constraints-colocation" msg-prefix="Constraints-colocation">
    <cibtr:replace what="score-attribute"
                   with=""
                   msg-extra="was actually never in effect"/>
    <cibtr:replace what="score-attribute-mangle"
                   with=""
                   msg-extra="was actually never in effect"/>
  </cibtr:table>

</cibtr:map>

<xsl:variable name="MapResourcesOperation"
              select="document('')/xsl:stylesheet
                        /cibtr:map/cibtr:table[
                          @for = 'resources-operation'
                        ]"/>

<xsl:variable name="MapConstraintsColocation"
              select="document('')/xsl:stylesheet
                        /cibtr:map/cibtr:table[
                          @for = 'constraints-colocation'
                        ]"/>

<xsl:template name="MapMsg">
  <xsl:param name="Context" select="''"/>
  <xsl:param name="Replacement"/>
  <xsl:choose>
    <xsl:when test="not($Replacement)"/>
    <xsl:when test="count($Replacement) != 1">
      <xsl:message terminate="yes">
        <xsl:value-of select="concat('INTERNAL ERROR:',
                                     $Replacement/../@msg-prefix,
                                     ': count($Replacement) != 1',
                                     ' does not hold (',
                                     count($Replacement), ')')"/>
      </xsl:message>
    </xsl:when>
    <xsl:otherwise>
      <xsl:variable name="MsgPrefix" select="concat(
                                               ($Replacement|$Replacement/..)
                                                 /@msg-prefix, ': '
                                             )"/>
      <xsl:message>
        <xsl:value-of select="$MsgPrefix"/>
        <xsl:if test="$Context">
          <xsl:value-of select="concat($Context, ': ')"/>
        </xsl:if>
        <xsl:choose>
          <xsl:when test="string($Replacement/@with)">
            <xsl:choose>
              <xsl:when test="string($Replacement/@where)">
                <xsl:value-of select="concat('moving ', $Replacement/@what,
                                             ' under ', $Replacement/@where)"/>
              </xsl:when>
              <xsl:when test="$Replacement/@with = $Replacement/@what">
                <xsl:value-of select="concat('keeping ', $Replacement/@what)"/>
              </xsl:when>
              <xsl:otherwise>
                <xsl:value-of select="concat('renaming ', $Replacement/@what)"/>
              </xsl:otherwise>
            </xsl:choose>
            <xsl:value-of select="concat(' as ', $Replacement/@with)"/>
            <xsl:if test="$Replacement/@where">
              <xsl:value-of select="' unless already defined there'"/>
            </xsl:if>
          </xsl:when>
          <xsl:otherwise>
            <xsl:value-of select="concat('dropping ', $Replacement/@what)"/>
          </xsl:otherwise>
        </xsl:choose>
        <xsl:if test="string($Replacement/@in-case-of)">
          <xsl:value-of select="concat(' for matching ',
                                       $Replacement/@in-case-of)"/>
        </xsl:if>
      </xsl:message>
      <xsl:if test="$Replacement/@msg-extra">
        <xsl:message>
          <xsl:value-of select="concat($MsgPrefix, '... ',
                                       $Replacement/@msg-extra)"/>
        </xsl:message>
      </xsl:if>
    </xsl:otherwise>
  </xsl:choose>
</xsl:template>

<xsl:template name="FilterNonattrOpMetaAttributes">
  <xsl:param name="Source"/>
  <xsl:param name="InverseMode" select="false()"/>
  <xsl:for-each select="$Source/node()">
    <xsl:choose>
      <xsl:when test="self::text()
                      and
                      not($InverseMode)">
        <!-- keep normalized for the denormalized-space indication
             to be observable -->
        <xsl:value-of select="normalize-space(.)"/>
      </xsl:when>
      <xsl:when test="self::nvpair">
        <xsl:variable name="Replacement"
                      select="$MapResourcesOperation/cibtr:replace[
                                @what = current()/@name
                                and
                                (
                                  (
                                    @in-case-of
                                    and
                                    contains(concat('|', @in-case-of, '|'),
                                             concat('|', current()/../../@name, '|'))
                                  )
                                  or
                                  (
                                    not(@in-case-of)
                                    and
                                    not(
                                      $MapResourcesOperation/cibtr:replace[
                                        @what = current()/@name
                                        and
                                        @in-case-of
                                        and
                                        contains(concat('|', @in-case-of, '|'),
                                                 concat('|', current()/../../@name, '|'))
                                      ]
                                    )
                                  )
                                )
                              ]"/>
        <xsl:if test="not($InverseMode)">
          <xsl:call-template name="MapMsg">
            <xsl:with-param name="Context" select="../../@id"/>
            <xsl:with-param name="Replacement" select="$Replacement"/>
          </xsl:call-template>
        </xsl:if>
        <xsl:choose>
          <xsl:when test="$Replacement
                          and
                          (
                            not($Replacement/@with)
                            or
                            $Replacement/@where
                          )">
            <!-- drop (possibly just move over) -->
            <xsl:if test="$Replacement/@where
                          and
                          $InverseMode">
              <!-- inject whitespace so we can distinguish some content is present
                   at all when pulling the result to variable for later test -->
              <xsl:text> </xsl:text>
              <xsl:copy>
                <xsl:apply-templates select="@*"/>
              </xsl:copy>
            </xsl:if>
          </xsl:when>
          <xsl:when test="$Replacement">
            <xsl:message terminate="yes">
              <xsl:value-of select="concat('INTERNAL ERROR: ',
                                           $Replacement/../@msg-prefix,
                                           ': no in-situ rename',
                                           ' does not hold (',
                                           not(($InverseMode)), ')')"/>
            </xsl:message>
          </xsl:when>
          <xsl:when test="$InverseMode"/>
          <xsl:otherwise>
            <!-- inject whitespace so we can distinguish some content is present
                 at all when pulling the result to variable for later test -->
            <xsl:text> </xsl:text>
            <!-- XXX mode so that whitespace is always normalized -->
            <xsl:copy>
              <xsl:apply-templates select="@*|node()"/>
            </xsl:copy>
          </xsl:otherwise>
        </xsl:choose>
      </xsl:when>
      <xsl:when test="$InverseMode
                      or
                      self::comment()"/>
      <xsl:otherwise>
        <xsl:copy>
          <xsl:apply-templates select="@*|node()"/>
        </xsl:copy>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:for-each>
</xsl:template>

<!--
 actual transformation
 -->

<xsl:template match="cib">
  <xsl:copy>
    <xsl:apply-templates select="@*"/>
    <xsl:attribute name="validate-with">
      <xsl:value-of select="concat('pacemaker-', $cib-min-ver)"/>
    </xsl:attribute>
    <xsl:apply-templates select="node()"/>
  </xsl:copy>
</xsl:template>

<xsl:template match="rsc_colocation">
  <xsl:copy>
    <xsl:for-each select="@*">
      <xsl:variable name="Replacement"
                    select="$MapConstraintsColocation/cibtr:replace[
                              @what = name(current())
                            ]"/>
      <xsl:call-template name="MapMsg">
        <xsl:with-param name="Context" select="../@id"/>
        <xsl:with-param name="Replacement" select="$Replacement"/>
      </xsl:call-template>
      <xsl:choose>
        <xsl:when test="$Replacement
                        and
                        not(string($Replacement/@with))">
          <!-- drop -->
        </xsl:when>
        <xsl:when test="$Replacement">
          <!-- rename -->
          <xsl:attribute name="{name()}">
            <xsl:value-of select="$Replacement/@with"/>
          </xsl:attribute>
        </xsl:when>
        <xsl:otherwise>
          <xsl:copy/>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:for-each>
    <xsl:apply-templates select="node()"/>
  </xsl:copy>
</xsl:template>

<!--
 1a. propagate (primitive|template)/operations/
       op[name() = 'start' or name() = 'promote']/@requires
     under new ./meta_attributes/nvpair

 1b. "move" (primitive|template)/operations/
       op[name() = 'start' or name() = 'promote']/
       meta_attributes/nvpair[@requires]
     under ./meta_attributes

  otherwise, just

 2a.  drop (primitive|template)/operations/
        op/@requires

 2b.  drop (primitive|template)/operations/
        op/meta_attributes/nvpair[@requires]

 Not compatible with meta_attributes referenced via id-ref
 (would need external preprocessing).
 -->
<xsl:template match="primitive|template">
  <xsl:copy>
    <xsl:apply-templates select="@*"/>

    <!-- B: special-casing operations -->
    <xsl:for-each select="node()">
      <xsl:choose>
        <xsl:when test="self::operations">
          <xsl:copy>
            <xsl:apply-templates select="@*"/>
            <!-- B: special-casing op -->
            <xsl:for-each select="node()">
              <xsl:copy>
                <xsl:choose>
                  <xsl:when test="self::op">
                    <xsl:for-each select="@*">
                      <xsl:variable name="Replacement"
                                    select="$MapResourcesOperation/cibtr:replace[
                                              @what = name(current())
                                              and
                                              (
                                                (
                                                  @in-case-of
                                                  and
                                                  contains(concat('|', @in-case-of, '|'),
                                                           concat('|', current()/../@name, '|'))
                                                )
                                                or
                                                (
                                                  not(@in-case-of)
                                                  and
                                                  not(
                                                    $MapResourcesOperation/cibtr:replace[
                                                      @what = name(current())
                                                      and
                                                      @in-case-of
                                                      and
                                                      contains(concat('|', @in-case-of, '|'),
                                                               concat('|', current()/../@name, '|'))
                                                    ]
                                                  )
                                                )
                                              )
                                            ]"/>
                      <xsl:call-template name="MapMsg">
                        <xsl:with-param name="Context" select="../../@id"/>
                        <xsl:with-param name="Replacement" select="$Replacement"/>
                      </xsl:call-template>
                      <xsl:choose>
                        <xsl:when test="$Replacement
                                        and
                                        (
                                          not($Replacement/@with)
                                          or
                                          $Replacement/@where
                                        )">
                          <!-- drop (possibly just move over) -->
                        </xsl:when>
                        <xsl:when test="$Replacement">
                          <xsl:message terminate="yes">
                            <xsl:value-of select="concat('INTERNAL ERROR: ',
                                                         $Replacement/../@msg-prefix,
                                                         ': no in-situ rename',
                                                         ' does not hold')"/>
                          </xsl:message>
                        </xsl:when>
                        <xsl:otherwise>
                          <xsl:copy/>
                        </xsl:otherwise>
                      </xsl:choose>
                    </xsl:for-each>
                    <!-- B: special-casing meta_attributes -->
                    <xsl:for-each select="node()">
                      <xsl:choose>
                        <xsl:when test="self::text()">
                          <!-- for cases when "op" becomes child-less -->
                          <xsl:value-of select="normalize-space(.)"/>
                        </xsl:when>
                        <xsl:when test="self::meta_attributes">
                          <xsl:variable name="FilteredOpMetaAttributes">
                            <xsl:call-template name="FilterNonattrOpMetaAttributes">
                              <xsl:with-param name="Source" select="."/>
                            </xsl:call-template>
                          </xsl:variable>
                          <xsl:if test="normalize-space($FilteredOpMetaAttributes)
                                        != $FilteredOpMetaAttributes">
                            <xsl:copy>
                              <xsl:apply-templates select="@*"/>
                              <xsl:copy-of select="$FilteredOpMetaAttributes"/>
                            </xsl:copy>
                          </xsl:if>
                        </xsl:when>
                        <xsl:otherwise>
                          <xsl:copy>
                            <xsl:apply-templates select="@*|node()"/>
                          </xsl:copy>
                        </xsl:otherwise>
                      </xsl:choose>
                    </xsl:for-each>
                    <!-- E: special-casing meta_attributes -->
                  </xsl:when>
                  <xsl:otherwise>
                    <xsl:apply-templates select="@*|node()"/>
                  </xsl:otherwise>
                </xsl:choose>
              </xsl:copy>
            </xsl:for-each>
            <!-- E: special-casing op -->
          </xsl:copy>
        </xsl:when>
        <xsl:otherwise>
          <xsl:copy>
            <xsl:apply-templates select="@*|node()"/>
          </xsl:copy>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:for-each>
    <!-- E: special-casing operations -->

    <!-- add as last meta_attributes block -->

    <xsl:variable name="ToMoveIndirect">
      <xsl:for-each select="operations/op/@*">
        <xsl:variable name="Replacement"
                      select="$MapResourcesOperation/cibtr:replace[
                                @what = name(current())
                                and
                                (
                                  (
                                    @in-case-of
                                    and
                                    contains(concat('|', @in-case-of, '|'),
                                             concat('|', current()/../@name, '|'))
                                  )
                                  or
                                  (
                                    not(@in-case-of)
                                    and
                                    not(
                                      $MapResourcesOperation/cibtr:replace[
                                        @what = name(current())
                                        and
                                        @in-case-of
                                        and
                                        contains(concat('|', @in-case-of, '|'),
                                                 concat('|', current()/../@name, '|'))
                                      ]
                                    )
                                  )
                                )
                              ]"/>
        <xsl:if test="$Replacement
                      and
                      $Replacement/@where = 'meta_attributes'">
          <!-- inject whitespace so we can distinguish some content is present
               at all when pulling the result to variable for later test -->
          <xsl:text> </xsl:text>
          <nvpair id="{concat('_2TO3_', ../@id, '-meta-', $Replacement/@with)}"
                  name="{$Replacement/@with}" value="{.}"/>
        </xsl:if>
      </xsl:for-each>
    </xsl:variable>
    <xsl:if test="normalize-space($ToMoveIndirect) != $ToMoveIndirect">
      <meta_attributes id="{concat('_2TO3_', @id, '-meta')}">
        <xsl:copy-of select="$ToMoveIndirect"/>
      </meta_attributes>
    </xsl:if>

    <xsl:variable name="ToMoveDirect">
      <xsl:for-each select="operations/op/@*">
        <xsl:variable name="Replacement"
                      select="$MapResourcesOperation/cibtr:replace[
                                @what = name(current())
                                and
                                (
                                  (
                                    @in-case-of
                                    and
                                    contains(concat('|', @in-case-of, '|'),
                                             concat('|', current(), '|'))
                                  )
                                  or
                                  (
                                    not(@in-case-of)
                                    and
                                    not(
                                      $MapResourcesOperation/cibtr:replace[
                                        @what = name(current())
                                        and
                                        @in-case-of
                                        and
                                        contains(concat('|', @in-case-of, '|'),
                                                 concat('|', current(), '|'))
                                      ]
                                    )
                                  )
                                )
                              ]"/>
        <xsl:if test="$Replacement
                      and
                      $Replacement/@where = 'meta_attributes'">
          <nvpair id="{concat('_2TO3_', ../@id, '-meta-', $Replacement/@with)}"
                  name="{$Replacement/@with}" value="{.}"/>
        </xsl:if>
      </xsl:for-each>
    </xsl:variable>

    <xsl:for-each select="operations/op/meta_attributes">
      <xsl:variable name="FilteredOpMetaAttributes">
        <xsl:call-template name="FilterNonattrOpMetaAttributes">
          <xsl:with-param name="Source" select="."/>
          <xsl:with-param name="InverseMode" select="true()"/>
        </xsl:call-template>
      </xsl:variable>
      <xsl:if test="normalize-space($FilteredOpMetaAttributes)
                    != $FilteredOpMetaAttributes">
        <xsl:copy>
          <xsl:apply-templates select="@*[
                                         name() != 'id'
                                       ]"/>
          <xsl:attribute name='id'>
            <xsl:value-of select="concat('_2TO3_', @id)"/>
          </xsl:attribute>
          <xsl:apply-templates select="node()[
                                         name() != 'nvpair'
                                       ]"/>
          <xsl:copy-of select="$FilteredOpMetaAttributes"/>
        </xsl:copy>
      </xsl:if>
    </xsl:for-each>

  </xsl:copy>
</xsl:template>

<xsl:template match="@*|node()">
  <xsl:copy>
    <xsl:apply-templates select="@*|node()"/>
  </xsl:copy>
</xsl:template>

</xsl:stylesheet>
